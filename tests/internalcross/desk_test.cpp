#include <raikiri/config/risk.hpp>
#include <raikiri/domain/demo_instruments.hpp>
#include <raikiri/internalcross/application/control.hpp>
#include <raikiri/internalcross/application/desk.hpp>
#include <raikiri/internalcross/domain/liability.hpp>
#include <raikiri/internalcross/infrastructure/file_store.hpp>
#include <raikiri/internalcross/infrastructure/journal_log.hpp>
#include <raikiri/internalcross/presentation/routes.hpp>
#include <raikiri/journal/reader.hpp>
#include <raikiri/oms/funding_split.hpp>
#include <raikiri/oms/recon.hpp>
#include <raikiri/oms/replay.hpp>
#include <raikiri/oms/risk.hpp>
#include <raikiri/portfolio/reconcile.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size); block != nullptr) {
        return block;
    }
    throw std::bad_alloc{};
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace raikiri::internalcross {
namespace {

constexpr std::uint16_t k_alice = 2000;
constexpr std::uint16_t k_bob = 2001;
constexpr std::uint16_t k_hedge = 3000;
constexpr UnixNanos k_now{1'000'000'000'000'000'000ULL};

struct Sink final : audit::Sink {
    struct Row {
        std::string actor;
        std::string action;
        std::string subject;
        std::string detail;
    };

    std::vector<Row> rows;

    void append(const audit::Entry& entry) noexcept override {
        rows.push_back({std::string{entry.actor},
                        std::string{entry.action},
                        std::string{entry.subject},
                        std::string{entry.detail_json}});
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(rows, [&](const Row& row) { return row.action == action; }));
    }
};

class Kernel final : public CrossKernel {
public:
    Kernel(oms::OrderStore& store, portfolio::PositionBook& positions) : store_{&store}, positions_{&positions} {}

    FixedE9 bid = FixedE9::from_raw(50'000'000'000'000);
    FixedE9 ask = FixedE9::from_raw(50'001'000'000'000);
    UnixNanos quote_ts = k_now;
    std::map<std::uint16_t, oms::BookState> states;
    std::vector<OrderEvent> heard;
    bool hedge_open{true};

    std::size_t touches(InstrumentId, std::span<Touch> out) override {
        out[0] = {.bid = bid, .ask = ask, .ts = quote_ts, .own = true};
        return 1;
    }

    LegFacts facts(std::uint16_t book, InstrumentId instrument, ClientOrderId leaving_out) const override {
        LegFacts out{};
        if (const auto* held = positions_->find({.strategy_id = book, .instrument = instrument}); held != nullptr) {
            out.position = held->qty();
        }
        store_->for_each_open([&](const oms::Order& order) {
            if (order.client_order_id().strategy_id() == book && order.instrument() == instrument &&
                order.client_order_id() != leaving_out) {
                auto& side = order.side() == Side::Buy ? out.working_buy : out.working_sell;
                side = QtyLots::from_raw(side.raw + order.leaves().raw);
            }
        });
        return out;
    }

    bool leg_allowed(const OrderCmd& leg, ClientOrderId standing) override {
        const auto book = leg.client_order_id.strategy_id();
        const auto facts_now = facts(book, leg.instrument, standing);
        oms::Exposure exposure{.position_lots = facts_now.position,
                               .working_buy_lots = facts_now.working_buy,
                               .working_sell_lots = facts_now.working_sell};
        if (const auto it = states.find(book); it != states.end()) {
            exposure.book_state = it->second;
        }
        return oms::RiskGate{}.check(leg, exposure).has_value();
    }

    BookKind kind_of(std::uint16_t book) const override { return book == k_hedge ? BookKind::Hedge : BookKind::Client; }

    std::string name_of(std::uint16_t book) const override {
        return book == k_alice ? "client:alice" : book == k_bob ? "client:bob" : "hedge";
    }

    std::optional<std::uint16_t> hedge_book() const override {
        return hedge_open ? std::optional{k_hedge} : std::nullopt;
    }

    void booked(const OrderEvent& leg, bool placed) override {
        if (placed) {
            heard.push_back(leg);
        }
    }

private:
    oms::OrderStore* store_;
    portfolio::PositionBook* positions_;
};

class Rig {
public:
    explicit Rig(const std::filesystem::path& dir)
        : writer{*journal::JournalWriter::open(dir.string(), "journal")}, exec{store}, trail{sink},
          kernel{store, positions}, log{writer}, desk{kernel, log, store, exec, positions, trail, sink} {}

    Sink sink;
    oms::OrderStore store;
    portfolio::PositionBook positions;
    journal::JournalWriter writer;
    oms::ExecPath exec;
    oms::OrderTrail trail;
    Kernel kernel;
    JournalCrossLog log;
    CrossDesk desk;
    std::vector<Instrument> instruments = demo::sim_instruments();

    [[nodiscard]] const Instrument& btc() const { return instruments[0]; }

    [[nodiscard]] InstrumentId id() const { return base_of(btc()).id; }

    ClientOrderId place(std::uint16_t book,
                        std::uint64_t seq,
                        Side side,
                        std::int64_t lots,
                        std::int64_t limit = 0,
                        TimeInForce tif = TimeInForce::Gtc,
                        OrderFlags flags = OrderFlags::None) {
        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = side;
        cmd.tif = tif;
        cmd.flags = flags;
        cmd.order_type = limit == 0 ? OrderType::Market : OrderType::Limit;
        cmd.instrument = id();
        cmd.client_order_id = ClientOrderId::make(book, seq);
        cmd.exchange_client_order_id = ids.next_exchange_client_id(k_now);
        cmd.price = limit == 0 ? PriceTicks::undef() : PriceTicks::from_raw(limit);
        cmd.qty = QtyLots::from_raw(lots);
        cmd.ts_created = k_now;
        EXPECT_TRUE(store.create(cmd, cmd.exchange_client_order_id).has_value());
        oms::append_order_cmd(writer, cmd);
        OrderEvent submitted{};
        submitted.type = OrderEventType::Submitted;
        submitted.client_order_id = cmd.client_order_id;
        submitted.instrument = cmd.instrument;
        submitted.side = side;
        submitted.ts_init = k_now;
        EXPECT_TRUE(store.apply_event(submitted).has_value());
        return cmd.client_order_id;
    }

    // a venue fill the way the kernel books one, so a book holds something a cross can draw on
    void venue_fill(std::uint16_t book, Side side, std::int64_t lots) {
        OrderEvent event{};
        event.type = OrderEventType::Filled;
        event.side = side;
        event.instrument = id();
        event.client_order_id = ClientOrderId::make(book, 9000 + (++venue_fills));
        event.price = PriceTicks::from_raw(500'000);
        event.qty = QtyLots::from_raw(lots);
        event.fee = FixedE9::from_raw(0);
        event.ts_event = k_now;
        EXPECT_TRUE(positions.on_fill(btc(), event, event.qty).has_value());
    }

    [[nodiscard]] std::int64_t held_lots(std::uint16_t book) const {
        const auto* held = positions.find({.strategy_id = book, .instrument = id()});
        return held != nullptr ? held->qty().raw : 0;
    }

    [[nodiscard]] std::int64_t book_sum() const {
        std::int64_t sum = 0;
        for (const auto& held : positions.all()) {
            sum += held.qty().raw;
        }
        return sum;
    }

    void on(std::uint64_t hold_ms = 0) {
        Settings settings{.enabled = true, .hold_ms = hold_ms, .against_desk = true};
        for (const auto* name : {"client:alice", "client:bob"}) {
            settings.books[name] = {.enabled = true, .instruments = {"BTC/USDT:perp"}};
        }
        desk.configure(settings);
    }

private:
    oms::IdGen ids{1, 42};
    std::uint64_t venue_fills{0};
};

class CrossTest : public testing::Test {
protected:
    CrossTest()
        : dir_{std::filesystem::temp_directory_path() /
               ("internal_cross_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))} {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    ~CrossTest() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(CrossTest, OffUnlessAPersonSaidSo) {
    Rig rig{dir_};
    EXPECT_FALSE(rig.desk.wants(k_alice, rig.btc()));
    rig.desk.configure({.enabled = true});
    EXPECT_FALSE(rig.desk.wants(k_alice, rig.btc()));
    rig.desk.configure({.books = {{"client:alice", {.enabled = true, .instruments = {"BTC/USDT:perp"}}}}});
    EXPECT_FALSE(rig.desk.wants(k_alice, rig.btc()));
    rig.on();
    EXPECT_TRUE(rig.desk.wants(k_alice, rig.btc()));
    EXPECT_FALSE(rig.desk.wants(k_hedge, rig.btc()));
    EXPECT_FALSE(rig.desk.wants(k_alice, rig.instruments[1]));
}

TEST_F(CrossTest, TwoClientsCrossAndBothOrdersAndBooksMove) {
    Rig rig{dir_};
    rig.on(100);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
    const auto first = rig.desk.on_arrival(buyer, rig.btc(), k_now);
    EXPECT_TRUE(first.held);
    EXPECT_EQ(first.crossed.raw, 0);

    const auto seller = rig.place(k_bob, 1, Side::Sell, 4);
    const auto second = rig.desk.on_arrival(seller, rig.btc(), k_now);
    EXPECT_EQ(second.crossed.raw, 4);
    EXPECT_FALSE(second.held);

    EXPECT_EQ(rig.held_lots(k_alice), 4);
    EXPECT_EQ(rig.held_lots(k_bob), -4);
    EXPECT_EQ(rig.book_sum(), 0);

    const auto* open = rig.store.find(buyer);
    ASSERT_NE(open, nullptr);
    EXPECT_EQ(open->leaves().raw, 6);
    EXPECT_EQ(open->filled().raw, 4);
    EXPECT_EQ(open->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(open->venue_qty().raw, 6);
    EXPECT_EQ(rig.store.find(seller), nullptr); // whole, so retired
    EXPECT_TRUE(rig.desk.holds(buyer));

    ASSERT_EQ(rig.kernel.heard.size(), 2U);
    EXPECT_EQ(rig.kernel.heard[0].liquidity, Liquidity::Internal);
    EXPECT_TRUE(rig.kernel.heard[0].exec_id.view().starts_with("ix"));
    EXPECT_NE(rig.kernel.heard[0].exec_id, rig.kernel.heard[1].exec_id);
    // mid of 50000.0 and 50001.0, on a 0.1 tick
    EXPECT_EQ(rig.kernel.heard[0].price.raw, 500'005);

    EXPECT_EQ(rig.sink.count("order.crossed"), 2U);
    const auto crossed = std::ranges::find_if(rig.sink.rows,
                                              [](const auto& row) { return row.action == "order.crossed"; });
    EXPECT_NE(crossed->detail.find(std::to_string(buyer.raw)), std::string::npos);
    EXPECT_NE(crossed->detail.find(std::to_string(seller.raw)), std::string::npos);
    EXPECT_NE(crossed->detail.find("ref_bid"), std::string::npos);
    EXPECT_EQ(rig.desk.stats().crosses, 1U);
    EXPECT_EQ(rig.desk.stats().against_clients, 1U);
    EXPECT_GT(rig.desk.stats().saved.raw, 0);

    const auto due = rig.desk.take_due(UnixNanos{k_now.ns + 100'000'000});
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0], buyer);
    EXPECT_FALSE(rig.desk.holds(buyer));
}

TEST_F(CrossTest, ZeroWindowHoldsNothing) {
    Rig rig{dir_};
    rig.on();
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
    const auto arrived = rig.desk.on_arrival(buyer, rig.btc(), k_now);
    EXPECT_FALSE(arrived.held);
    EXPECT_EQ(arrived.crossed.raw, 0);
    EXPECT_EQ(arrived.why, Refusal::NoCounterparty);
    EXPECT_FALSE(rig.desk.holds(buyer));
}

TEST_F(CrossTest, TheDeskSellsDownItsInventoryOnly) {
    Rig rig{dir_};
    rig.on();
    rig.venue_fill(k_hedge, Side::Buy, 6);
    const auto sum_before = rig.book_sum();

    const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
    const auto arrived = rig.desk.on_arrival(buyer, rig.btc(), k_now);
    EXPECT_EQ(arrived.crossed.raw, 6);
    EXPECT_EQ(rig.held_lots(k_hedge), 0);
    EXPECT_EQ(rig.held_lots(k_alice), 6);
    EXPECT_EQ(rig.book_sum(), sum_before);
    EXPECT_EQ(rig.store.find(buyer)->leaves().raw, 4);
    EXPECT_EQ(rig.desk.stats().against_desk, 1U);
    // the hedge leg is nobody's order to hear about
    ASSERT_EQ(rig.kernel.heard.size(), 1U);
    EXPECT_EQ(rig.kernel.heard[0].client_order_id, buyer);

    // a client selling into a desk that holds nothing would grow it
    const auto seller = rig.place(k_bob, 1, Side::Sell, 3);
    EXPECT_EQ(rig.desk.on_arrival(seller, rig.btc(), k_now).crossed.raw, 0);
    EXPECT_EQ(rig.held_lots(k_hedge), 0);
}

TEST_F(CrossTest, TheHedgeBookIsNotTouchedUnlessSaid) {
    Rig rig{dir_};
    rig.on();
    auto settings = rig.desk.settings();
    settings.against_desk = false;
    rig.desk.configure(settings);
    rig.venue_fill(k_hedge, Side::Buy, 6);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 2);
    const auto arrived = rig.desk.on_arrival(buyer, rig.btc(), k_now);
    EXPECT_EQ(arrived.crossed.raw, 0);
    EXPECT_EQ(arrived.why, Refusal::NoCounterparty);
    EXPECT_EQ(rig.held_lots(k_hedge), 6);
}

TEST_F(CrossTest, RefusesBadBooksAndGoesToTheVenue) {
    Rig rig{dir_};
    rig.on();
    rig.venue_fill(k_hedge, Side::Buy, 6);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 2);
    rig.kernel.quote_ts = UnixNanos{k_now.ns - 5'000'000'000};
    auto arrived = rig.desk.on_arrival(buyer, rig.btc(), k_now);
    EXPECT_EQ(arrived.why, Refusal::StaleQuote);
    EXPECT_EQ(arrived.crossed.raw, 0);
    rig.kernel.quote_ts = k_now;
    rig.kernel.ask = rig.kernel.bid;
    EXPECT_EQ(rig.desk.on_arrival(buyer, rig.btc(), k_now).why, Refusal::CrossedQuote);
    rig.kernel.ask = FixedE9::from_raw(60'000'000'000'000);
    EXPECT_EQ(rig.desk.on_arrival(buyer, rig.btc(), k_now).why, Refusal::WideQuote);
    EXPECT_EQ(rig.held_lots(k_alice), 0);
    EXPECT_EQ(rig.desk.stats().refused[static_cast<std::size_t>(Refusal::StaleQuote)], 1U);
}

TEST_F(CrossTest, AHaltedOrReducingCounterpartyIsPassedOver) {
    Rig rig{dir_};
    rig.on(100);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
    (void)rig.desk.on_arrival(buyer, rig.btc(), k_now);

    rig.kernel.states[k_alice] = oms::BookState::Halted;
    const auto seller = rig.place(k_bob, 1, Side::Sell, 4);
    auto arrived = rig.desk.on_arrival(seller, rig.btc(), k_now);
    EXPECT_EQ(arrived.crossed.raw, 0);
    EXPECT_EQ(arrived.why, Refusal::Risk);
    EXPECT_EQ(rig.held_lots(k_bob), 0);

    // reducing, and the buy would grow it
    rig.kernel.states[k_alice] = oms::BookState::Reducing;
    const auto other = rig.place(k_bob, 2, Side::Sell, 4);
    arrived = rig.desk.on_arrival(other, rig.btc(), k_now);
    EXPECT_EQ(arrived.crossed.raw, 0);
    EXPECT_EQ(arrived.why, Refusal::Risk);

    // a hedge book that may not trade gives nothing either
    rig.venue_fill(k_hedge, Side::Buy, 6);
    rig.kernel.states[k_hedge] = oms::BookState::Halted;
    const auto buyer_two = rig.place(k_bob, 3, Side::Buy, 2);
    EXPECT_EQ(rig.desk.on_arrival(buyer_two, rig.btc(), k_now).crossed.raw, 0);
}

TEST_F(CrossTest, PostOnlyAndFillOrKillRules) {
    Rig rig{dir_};
    rig.on();
    rig.venue_fill(k_hedge, Side::Buy, 3);
    const auto post = rig.place(k_alice, 77, Side::Buy, 1, 500'000, TimeInForce::Gtc, OrderFlags::PostOnly);
    EXPECT_EQ(rig.desk.on_arrival(post, rig.btc(), k_now).why, Refusal::PostOnly);

    const auto kill = rig.place(k_alice, 78, Side::Buy, 5, 0, TimeInForce::Fok);
    const auto fok = rig.desk.on_arrival(kill, rig.btc(), k_now);
    EXPECT_EQ(fok.why, Refusal::FokShort);
    EXPECT_EQ(fok.crossed.raw, 0);
    EXPECT_EQ(rig.held_lots(k_hedge), 3);
}

TEST_F(CrossTest, TheVenueSeesOnlyWhatDidNotCross) {
    // every order the venue's events can arrive in, on top of a cross
    std::array<int, 4> order{0, 1, 2, 3};
    do {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        Rig rig{dir_};
        rig.on();
        rig.venue_fill(k_hedge, Side::Buy, 4);
        const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
        ASSERT_EQ(rig.desk.on_arrival(buyer, rig.btc(), k_now).crossed.raw, 4);
        ASSERT_EQ(rig.store.find(buyer)->venue_qty().raw, 6);

        OrderEvent accepted{};
        accepted.type = OrderEventType::Accepted;
        accepted.client_order_id = buyer;
        accepted.exchange_order_id = *ExchangeOrderId::from("v1");
        accepted.ts_init = k_now;
        const auto fill_of = [&](std::string_view exec, std::int64_t lots, std::int64_t leaves) {
            OrderEvent event{};
            event.type = OrderEventType::Filled;
            event.side = Side::Buy;
            event.client_order_id = buyer;
            event.instrument = rig.id();
            event.exec_id = *ExecId::from(exec);
            event.price = PriceTicks::from_raw(500'010);
            event.qty = QtyLots::from_raw(lots);
            event.leaves_qty = QtyLots::from_raw(leaves);
            event.ts_init = k_now;
            return event;
        };
        // leaves as the venue counts them: from the 6 it was told
        const std::array events{accepted, fill_of("v-a", 2, 4), fill_of("v-b", 3, 1), fill_of("v-c", 1, 0)};
        std::int64_t booked = 0;
        for (const auto i : order) {
            auto event = events[static_cast<std::size_t>(i)];
            const auto done = rig.exec.apply(event, false, k_now);
            EXPECT_FALSE(done.refused) << i;
            booked += done.delta.raw;
        }
        // the venue's own three fills are the 6, whatever their order said on the way
        EXPECT_EQ(booked, 6);
        EXPECT_EQ(rig.store.find(buyer), nullptr);
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(CrossTest, AnExecutionIsBookedOnce) {
    Rig rig{dir_};
    rig.on(100);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 10);
    (void)rig.desk.on_arrival(buyer, rig.btc(), k_now);
    const auto seller = rig.place(k_bob, 1, Side::Sell, 4);
    (void)rig.desk.on_arrival(seller, rig.btc(), k_now);
    auto again = rig.kernel.heard[0];
    const auto done = rig.exec.apply(again, false, k_now);
    EXPECT_TRUE(done.repeat);
    EXPECT_EQ(rig.store.find(buyer)->leaves().raw, 6);
}

TEST_F(CrossTest, ReconcileSeesTheSameAccountAfterACross) {
    Rig rig{dir_};
    rig.on(100);
    // the account at the venue is long 6 because of the hedge book
    rig.venue_fill(k_hedge, Side::Buy, 6);
    const PositionReport venue{.net_qty = QtyLots::from_raw(6)};
    const auto verdict = [&] {
        return portfolio::reconcile_position(true, venue, QtyLots::from_raw(rig.book_sum())).verdict;
    };
    EXPECT_EQ(verdict(), portfolio::ReconVerdict::Agree);

    const auto alice = rig.place(k_alice, 1, Side::Buy, 10);
    (void)rig.desk.on_arrival(alice, rig.btc(), k_now); // 6 from the desk, 4 wait
    const auto bob = rig.place(k_bob, 1, Side::Sell, 4);
    (void)rig.desk.on_arrival(bob, rig.btc(), k_now);

    EXPECT_EQ(rig.held_lots(k_hedge), 0);
    EXPECT_EQ(rig.held_lots(k_alice), 10);
    EXPECT_EQ(rig.held_lots(k_bob), -4);
    // nothing happened at the venue: 10 - 4 + 0 is still its 6
    EXPECT_EQ(verdict(), portfolio::ReconVerdict::Agree);

    const auto rows = liability_rows(rig.positions.all(), [&](std::uint16_t book) { return rig.kernel.kind_of(book); });
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].client_long, 10);
    EXPECT_EQ(rows[0].client_short, 4);
    EXPECT_EQ(rows[0].book_sum(), 6);
    // four of the clients' lots have nothing at the venue behind them but each other
    EXPECT_EQ(rows[0].offset(), 4);

    // the rewind is over venue fills only: a cross recorded there would move a comparison a venue never saw
    const std::vector<oms::BookedFill> venue_fills{{.ts_ns = k_now.ns, .lots = 6}};
    EXPECT_EQ(oms::book_as_of(rig.book_sum(), venue_fills, k_now.ns - 1), 0);
}

TEST_F(CrossTest, FundingOnTheNetIsSplitByEachBooksOwnSize) {
    // venue net long 6, charged 60: long 10 owes 100, short 4 earns 40
    const std::array holders{oms::FundingHolder{.strategy_id = k_alice, .lots = 10},
                             oms::FundingHolder{.strategy_id = k_bob, .lots = -4}};
    const auto shares = oms::split_funding_net(holders, FixedE9::from_raw(60), 1);
    ASSERT_EQ(shares.size(), 2U);
    EXPECT_EQ(shares[0].amount.raw, 100);
    EXPECT_EQ(shares[1].amount.raw, -40);
    EXPECT_EQ(shares[0].amount.raw + shares[1].amount.raw, 60);
    // books facing each other net to nothing, and a stray charge still lands somewhere
    const std::array flat{oms::FundingHolder{.strategy_id = k_alice, .lots = 4},
                          oms::FundingHolder{.strategy_id = k_bob, .lots = -4}};
    const auto fallback = oms::split_funding_net(flat, FixedE9::from_raw(10), 1);
    ASSERT_EQ(fallback.size(), 2U);
    EXPECT_EQ(fallback[0].amount.raw + fallback[1].amount.raw, 10);
}

TEST_F(CrossTest, ARestartGetsTheSameAnswer) {
    std::map<std::uint16_t, portfolio::Position> live;
    ClientOrderId alice{};
    {
        Rig rig{dir_};
        rig.on(100);
        rig.venue_fill(k_hedge, Side::Buy, 3);
        alice = rig.place(k_alice, 1, Side::Buy, 10);
        ASSERT_EQ(rig.desk.on_arrival(alice, rig.btc(), k_now).crossed.raw, 3);
        const auto bob = rig.place(k_bob, 1, Side::Sell, 2);
        ASSERT_EQ(rig.desk.on_arrival(bob, rig.btc(), k_now).crossed.raw, 2);
        for (const auto& position : rig.positions.all()) {
            live.emplace(position.key().strategy_id, position);
        }
        rig.writer.flush();
    }

    oms::OrderStore store;
    oms::ExecPath exec{store};
    oms::OrderReplay replay{store, exec};
    oms::replay_orders(dir_.string(), "journal", replay);

    ASSERT_EQ(replay.crosses().size(), 2U);
    const auto* restored = store.find(alice);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->leaves().raw, 5);
    EXPECT_EQ(restored->filled().raw, 5);
    EXPECT_EQ(restored->venue_qty().raw, 5);
    EXPECT_EQ(restored->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(store.open_count(), 1U); // the hedge leg and bob closed whole
    EXPECT_TRUE(replay.unechoed_legs().empty());
    EXPECT_GE(replay.last_internal_leg(), 1U);
    // no number of a hedge leg gets into the book's own count
    EXPECT_EQ(replay.last_seq(k_hedge), 0U);

    // what restore_book seats, leg by leg
    std::map<std::uint16_t, oms::CrossLeg> seated;
    for (const auto& record : replay.crosses()) {
        seated[record.buy.order.strategy_id()] = record.buy;
        seated[record.sell.order.strategy_id()] = record.sell;
        EXPECT_EQ(record.qty.raw > 0, true);
    }
    for (const auto book : {k_alice, k_bob, k_hedge}) {
        ASSERT_TRUE(seated.contains(book));
        const auto& before = live.at(book);
        EXPECT_EQ(seated[book].net_qty, before.qty());
        EXPECT_EQ(seated[book].cost, before.cost());
        EXPECT_EQ(seated[book].realized, before.realized());
        EXPECT_EQ(seated[book].fees, before.fees());
        EXPECT_EQ(seated[book].fill_count, before.fill_count());
    }

    // the same executions offered again are the same money
    auto again = oms::cross_leg_event(replay.crosses().back(), Side::Buy);
    EXPECT_TRUE(exec.apply(again, false, k_now).repeat);
}

TEST_F(CrossTest, ACrashAfterTheCrossFrameLosesNoLeg) {
    std::filesystem::create_directories(dir_ / "torn");
    ClientOrderId buyer = ClientOrderId::make(k_alice, 1);
    ClientOrderId seller = ClientOrderId::make(k_bob, 1);
    {
        auto writer = *journal::JournalWriter::open((dir_ / "torn").string(), "journal");
        OrderCmd cmd{};
        cmd.kind = CmdKind::Create;
        cmd.side = Side::Buy;
        cmd.instrument = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{0});
        cmd.client_order_id = buyer;
        cmd.qty = QtyLots::from_raw(5);
        cmd.ts_created = k_now;
        oms::append_order_cmd(writer, cmd);
        cmd.side = Side::Sell;
        cmd.client_order_id = seller;
        oms::append_order_cmd(writer, cmd);
        oms::CrossRecord record{};
        record.instrument = cmd.instrument;
        record.price = PriceTicks::from_raw(500'005);
        record.qty = QtyLots::from_raw(5);
        record.ts = k_now;
        record.buy = {.order = buyer, .net_qty = QtyLots::from_raw(5), .fill_count = 1};
        record.sell = {.order = seller, .net_qty = QtyLots::from_raw(-5), .fill_count = 1};
        oms::append_cross(writer, record);
        // and the process dies before the legs' event frames
        writer.flush();
    }
    oms::OrderStore store;
    oms::ExecPath exec{store};
    oms::OrderReplay replay{store, exec};
    oms::replay_orders((dir_ / "torn").string(), "journal", replay);
    EXPECT_EQ(store.open_count(), 0U);
    EXPECT_EQ(replay.crosses().size(), 1U);
    const auto missing = replay.unechoed_legs();
    ASSERT_EQ(missing.size(), 2U);
    EXPECT_EQ(missing[0].liquidity, Liquidity::Internal);
    EXPECT_EQ(missing[0].qty.raw, 5);
}

TEST_F(CrossTest, AnOrderNobodyCrossesCostsNoAllocationOnceWarm) {
    Rig rig{dir_};
    rig.on();
    rig.venue_fill(k_hedge, Side::Sell, 2);
    const auto buyer = rig.place(k_alice, 1, Side::Buy, 2);
    (void)rig.desk.on_arrival(buyer, rig.btc(), k_now);
    const auto before = g_allocations.load();
    for (int i = 0; i < 1000; ++i) {
        const auto arrived = rig.desk.on_arrival(buyer, rig.btc(), k_now);
        EXPECT_EQ(arrived.crossed.raw, 0);
        EXPECT_TRUE(rig.desk.wants(k_alice, rig.btc()));
    }
    EXPECT_EQ(g_allocations.load(), before);
}

TEST(CrossRiskFile, OffWhenAbsentAndReadWhenNamed) {
    const auto dir = std::filesystem::temp_directory_path() / "internal_cross_risk_file";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "risk.yaml").string();
    { std::ofstream{path} << "defaults:\n  max_open_orders: 8\n"; }
    auto file = config::load_risk(path);
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_FALSE(file->internal_cross.enabled);
    EXPECT_FALSE(file->internal_cross.against_desk);
    EXPECT_TRUE(file->internal_cross.books.empty());
    {
        std::ofstream{path} << "defaults:\n  max_open_orders: 8\n"
                               "internal_cross:\n  enabled: true\n  hold_ms: 50\n  books:\n"
                               "    \"client:acme\":\n      enabled: true\n      fee_bp: 1\n"
                               "      instruments: [\"BTC/USDT:perp\"]\n";
    }
    file = config::load_risk(path);
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_TRUE(file->internal_cross.enabled);
    EXPECT_EQ(file->internal_cross.hold_ms, 50U);
    ASSERT_TRUE(file->internal_cross.books.contains("client:acme"));
    EXPECT_EQ(file->internal_cross.books.at("client:acme").instruments.at(0), "BTC/USDT:perp");
    std::filesystem::remove_all(dir);
}

TEST_F(CrossTest, SettingsAreSavedAuditedAndNeedConfirming) {
    Rig rig{dir_};
    FileSettingsStore store{(dir_ / "risk" / "internal_cross.json").string()};
    TestClock clock;
    CrossControl control{rig.desk, store, rig.sink, clock};
    ASSERT_TRUE(control.load({}).has_value());
    EXPECT_FALSE(control.settings().enabled);

    const auto refused = control.set({.enabled = true}, {.by = "vs", .reason = "try"});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, ControlError::NeedsConfirm);
    EXPECT_FALSE(control.settings().enabled);
    EXPECT_FALSE(control.set({.enabled = true}, {.confirm = true}).has_value());

    ASSERT_TRUE(
        control.set({.enabled = true}, {.by = "vs", .reason = "a \"quoted\" why", .confirm = true}).has_value());
    ASSERT_TRUE(control
                    .set_book("client:alice",
                              {.enabled = true, .instruments = {"BTC/USDT:perp"}},
                              {.by = "vs", .confirm = true})
                    .has_value());
    EXPECT_EQ(rig.sink.count("risk.cross.set"), 2U);
    EXPECT_EQ(rig.sink.rows.back().subject, "book:client:alice");
    EXPECT_TRUE(rig.desk.wants(k_alice, rig.btc()));
    EXPECT_FALSE(control.set({.hold_ms = 5000}, {.by = "vs", .confirm = true}).has_value());
    EXPECT_FALSE(control.set_book("hedge", {}, {.by = "vs"}).has_value());

    std::filesystem::create_directories(dir_ / "fresh");
    Rig fresh{dir_ / "fresh"};
    CrossControl reloaded{fresh.desk, store, fresh.sink, clock};
    ASSERT_TRUE(reloaded.load({}).has_value());
    EXPECT_TRUE(reloaded.settings().enabled);
    EXPECT_TRUE(fresh.desk.wants(k_alice, fresh.btc()));
}

TEST_F(CrossTest, ARiskFileIsTheWordOnlyUntilAPersonSpeaks) {
    Rig rig{dir_};
    FileSettingsStore store{(dir_ / "internal_cross.json").string()};
    TestClock clock;
    CrossControl control{rig.desk, store, rig.sink, clock};
    ASSERT_TRUE(control.load({.enabled = true, .hold_ms = 50}).has_value());
    EXPECT_TRUE(control.settings().enabled);
    ASSERT_TRUE(control.set({.enabled = false}, {.by = "vs"}).has_value());
    ASSERT_TRUE(control.load({.enabled = true, .hold_ms = 50}).has_value());
    EXPECT_FALSE(control.settings().enabled);
}

TEST_F(CrossTest, TheRouteShowsTheDeskAndRefusesWithoutAYes) {
    Rig rig{dir_};
    FileSettingsStore store{(dir_ / "internal_cross.json").string()};
    TestClock clock;
    CrossControl control{rig.desk, store, rig.sink, clock};
    ASSERT_TRUE(control.load({}).has_value());
    const CrossRoutes routes{control,
                             [] { return std::vector<LiabilityView>{}; },
                             [](std::uint16_t) { return std::string{"client:alice"}; }};
    http::Request get{};
    get.method = "GET";
    get.path = "/risk";
    EXPECT_FALSE(routes(get).has_value());
    get.path = "/risk/internal";
    const auto shown = routes(get);
    ASSERT_TRUE(shown.has_value());
    EXPECT_NE(shown->body.find(R"("enabled":false)"), std::string::npos);

    http::Request post{};
    post.method = "POST";
    post.path = "/risk/internal";
    post.body = R"({"enabled":true,"by":"vs"})";
    const auto asked = routes(post);
    ASSERT_TRUE(asked.has_value());
    EXPECT_EQ(asked->status, 409);
    EXPECT_NE(asked->body.find("turns internal crossing on"), std::string::npos);
    post.body = R"({"enabled":true,"by":"vs","confirm":true})";
    EXPECT_EQ(routes(post)->status, 200);
    post.path = "/risk/internal/books/client:alice";
    post.body = R"({"enabled":true,"instruments":["BTC/USDT:perp"],"by":"vs","confirm":true})";
    const auto booked = routes(post);
    EXPECT_EQ(booked->status, 200);
    EXPECT_NE(booked->body.find("BTC/USDT:perp"), std::string::npos);
}

} // namespace
} // namespace raikiri::internalcross
