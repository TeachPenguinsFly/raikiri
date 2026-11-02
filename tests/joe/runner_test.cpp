#include <raikiri/books/registry.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/runner.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/portfolio/position_book.hpp>
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/registry.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::joe {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;

FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

InstrumentId btc_on_binance() {
    return InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{7});
}

Instrument btc_perp() {
    InstrumentBase base{};
    base.id = btc_on_binance();
    base.tick_size = d("0.01");
    base.lot_size = d("0.001");
    base.contract_value = d("1");
    return PerpInstrument{base, 28'800};
}

class Recording final : public strategy::Context {
public:
    std::expected<ClientOrderId, strategy::RejectReason> submit(const strategy::NewOrder& order) noexcept override {
        orders.push_back(order);
        return ClientOrderId{orders.size()};
    }

    bool cancel(ClientOrderId id) noexcept override {
        cancels.push_back(id.raw);
        return true;
    }

    bool amend(ClientOrderId id, PriceTicks price) noexcept override {
        amends.emplace_back(id.raw, price.raw);
        return amend_goes;
    }

    void cancel_all(InstrumentId) noexcept override {}

    std::optional<Bbo> bbo(InstrumentId) const noexcept override { return std::nullopt; }

    const Instrument* instrument(InstrumentId) const noexcept override { return &listed; }

    const portfolio::Position* position(InstrumentId) const noexcept override { return nullptr; }

    FixedE9 unrealized(InstrumentId, PriceTicks) const noexcept override { return FixedE9::from_raw(0); }

    std::optional<ClientOrderId> set_target(const portfolio::PortfolioTarget&) noexcept override {
        return std::nullopt;
    }

    void set_timer(std::uint32_t, std::uint64_t) noexcept override {}

    const Clock& clock() const noexcept override { return clock_; }

    Instrument listed{btc_perp()};
    std::vector<strategy::NewOrder> orders;
    std::vector<std::uint64_t> cancels;
    std::vector<std::pair<std::uint64_t, std::int64_t>> amends;
    bool amend_goes{true};

private:
    LiveClock clock_;
};

TEST(ContextSink, ASlicesRepeatKeyReachesTheOrderManager) {
    Recording ctx;
    ContextSink sink;
    sink.attach(&ctx);
    ASSERT_TRUE(sink.submit({.instrument = base_of(ctx.listed).id,
                             .side = Side::Buy,
                             .lots = 1,
                             .limit = base_of(ctx.listed).tick_size,
                             .tif = ChildTif::Gtc,
                             .repeat_key = 77})
                    .has_value());
    EXPECT_EQ(ctx.orders.at(0).repeat_key, 77U);
}

TEST(JoeContextSink, MapsEachChildToItsTimeInForceAndFlags) {
    Recording ctx;
    ContextSink sink;
    sink.attach(&ctx);
    const ChildOrder child{.instrument = btc_on_binance(), .lots = 5, .limit = d("100.01")};
    (void)sink.submit(child);
    auto rests = child;
    rests.tif = ChildTif::Gtc;
    (void)sink.submit(rests);
    rests.tif = ChildTif::PostOnly;
    (void)sink.submit(rests);
    ASSERT_EQ(ctx.orders.size(), 3U);
    EXPECT_EQ(ctx.orders[0].tif, TimeInForce::Ioc);
    EXPECT_EQ(ctx.orders[0].flags, OrderFlags::None);
    EXPECT_EQ(ctx.orders[1].tif, TimeInForce::Gtc);
    EXPECT_EQ(ctx.orders[1].flags, OrderFlags::None);
    EXPECT_EQ(ctx.orders[2].tif, TimeInForce::Gtc);
    EXPECT_EQ(ctx.orders[2].flags, OrderFlags::PostOnly);
    EXPECT_EQ(ctx.orders[2].price.raw, 10001);
}

TEST(JoeContextSink, AModifyAsksTheKernelInTicksAndSaysWhenItDidNotGo) {
    Recording ctx;
    ContextSink sink;
    EXPECT_FALSE(sink.modify(9, btc_on_binance(), d("100")));
    sink.attach(&ctx);
    EXPECT_TRUE(sink.modify(9, btc_on_binance(), d("100.02")));
    ASSERT_EQ(ctx.amends.size(), 1U);
    EXPECT_EQ(ctx.amends[0], (std::pair<std::uint64_t, std::int64_t>{9, 10002}));
    ctx.amend_goes = false;
    EXPECT_FALSE(sink.modify(9, btc_on_binance(), d("100.03")));
}

class Market final : public MarketView {
public:
    std::vector<VenueBook> books(const Listing&, Side side) const override {
        return {{.instrument = btc_on_binance(),
                 .lot = d("0.001"),
                 .levels = {{side == Side::Buy ? ask : bid, d("1")}},
                 .amend = true}};
    }

    std::optional<Scale> scale(InstrumentId) const override {
        return Scale{.listing = {.symbol = *SymbolText::from("BTC/USDT")}, .tick = d("0.01"), .lot = d("0.001")};
    }

    FixedE9 bid{d("99.98")};
    FixedE9 ask{d("100")};
};

struct Stack {
    Market market;
    ContextSink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Desk desk{market, sink, clock, curves};
    Runner runner{desk, sink};
    Recording ctx;

    Stack() { runner.on_start(ctx); }

    std::uint64_t start() {
        ParentSpec spec{};
        spec.listing.symbol = *SymbolText::from("BTC/USDT");
        spec.qty = d("0.05");
        spec.algo = Algo::Pegged;
        spec.max_child_notional = d("1000");
        return *desk.start(spec);
    }

    void event(OrderEventType type, std::int64_t qty = 0, std::int64_t leaves = 0) {
        OrderEvent out{};
        out.type = type;
        out.instrument = btc_on_binance();
        out.client_order_id = ClientOrderId{1};
        out.qty = QtyLots::from_raw(qty);
        out.leaves_qty = QtyLots::from_raw(leaves);
        out.price = PriceTicks::from_raw(9998);
        out.fee = d("0");
        runner.on_order_event(ctx, out);
    }
};

TEST(JoeRunner, CarriesTheAmendAndCancelAnswersThrough) {
    Stack s;
    const auto id = s.start();
    s.event(OrderEventType::Accepted);
    s.market.bid = d("100");
    s.market.ask = d("100.02");
    s.clock.advance(1 * k_s);
    s.desk.tick();
    ASSERT_EQ(s.ctx.amends.size(), 1U);

    s.event(OrderEventType::Updated);
    const auto& parent = *s.desk.find(id);
    EXPECT_EQ(parent.children()[0].limit, d("100"));

    s.market.bid = d("100.05");
    s.market.ask = d("100.07");
    s.clock.advance(1 * k_s);
    s.desk.tick();
    ASSERT_EQ(s.ctx.amends.size(), 2U);
    s.event(OrderEventType::ModifyRejected);
    EXPECT_EQ(parent.children()[0].limit, d("100"));

    s.clock.advance(1 * k_s);
    s.desk.tick();
    ASSERT_EQ(s.ctx.cancels.size(), 1U);
    EXPECT_TRUE(parent.children()[0].cancel_sent.ns != 0);
    s.event(OrderEventType::CancelRejected);
    EXPECT_TRUE(parent.children()[0].cancel_sent.ns == 0);
}

TEST(JoeRunner, AFillBooksTheDropInLeavesAndACloseFreesTheSlot) {
    Stack s;
    const auto id = s.start();
    s.event(OrderEventType::Accepted);
    s.event(OrderEventType::Filled, 5, 45);
    const auto& parent = *s.desk.find(id);
    EXPECT_EQ(parent.filled(), d("0.005"));
    s.market.bid = d("100");
    s.market.ask = d("100.02");
    s.clock.advance(1 * k_s);
    s.desk.tick();
    ASSERT_EQ(s.ctx.cancels.size(), 1U);
    EXPECT_TRUE(s.ctx.amends.empty());
    s.event(OrderEventType::Canceled);
    ASSERT_EQ(s.ctx.orders.size(), 2U);
    EXPECT_EQ(s.ctx.orders[1].qty.raw, 45);
    EXPECT_EQ(parent.live()->id, 2U);
}

// strategyd books a fill into the position by oms::filled_delta; a child must book the same
TEST(JoeLateFill, AChildBooksWhatThePositionBooks) {
    struct Case {
        std::int64_t prior;
        std::int64_t qty;
        std::int64_t leaves;
    };

    const auto undef = QtyLots::undef().raw;
    const std::vector<Case> cases{{0, 10, 90},
                                  {0, 10, 70},
                                  {30, 10, 70},
                                  {30, 10, 60},
                                  {30, 10, 80},
                                  {0, 100, 0},
                                  {0, 250, 0},
                                  {0, 10, undef},
                                  {90, 25, undef},
                                  {100, 5, undef},
                                  {100, 5, 0}};
    const oms::Order order{};
    for (const auto& c : cases) {
        ParentSpec spec{};
        spec.listing.symbol = *SymbolText::from("BTC/USDT");
        Parent parent{1, spec, UnixNanos{1}};
        ASSERT_TRUE(parent.start(UnixNanos{2}, d("1"), d("100")).has_value());
        parent.add({.id = 7, .instrument = btc_on_binance(), .lot = d("0.001"), .lots = 100, .limit = d("100")});
        if (c.prior > 0) {
            ASSERT_TRUE(parent.fill(UnixNanos{3}, 7, d("0.001").scaled_by(c.prior), d("100"), d("0")));
        }
        const auto before = parent.filled();

        OrderEvent event{};
        event.qty = QtyLots::from_raw(c.qty);
        event.leaves_qty = QtyLots::from_raw(c.leaves);
        const auto position = oms::filled_delta({.order = &order, .prev_leaves = QtyLots::from_raw(100 - c.prior)},
                                                event);

        const auto leaves = c.leaves == undef ? FixedE9::undef() : d("0.001").scaled_by(c.leaves);
        ASSERT_TRUE(parent.fill(UnixNanos{4}, 7, d("0.001").scaled_by(c.qty), d("100"), d("0"), leaves));
        EXPECT_EQ((parent.filled() - before).raw, d("0.001").scaled_by(position.raw).raw)
            << "prior " << c.prior << " qty " << c.qty << " leaves " << c.leaves;
    }
}

// a kernel that mints children the way strategyd does: the book in the id, the origin beside it
class Kernel final : public strategy::Context {
public:
    std::expected<ClientOrderId, strategy::RejectReason> submit(const strategy::NewOrder&) noexcept override {
        ADD_FAILURE() << "joe's children go through submit_child";
        return std::unexpected{strategy::RejectReason::Risk};
    }

    std::expected<ClientOrderId, strategy::RejectReason>
    submit_child(const strategy::NewOrder& order, std::uint16_t book, const OrderOrigin& origin) noexcept override {
        sent.push_back(ClientOrderId::make_child(book, sent.size() + 1));
        orders.push_back(order);
        origins.push_back(origin);
        return sent.back();
    }

    bool cancel(ClientOrderId id) noexcept override {
        cancels.push_back(id);
        return true;
    }

    void cancel_all(InstrumentId) noexcept override {}

    std::optional<Bbo> bbo(InstrumentId) const noexcept override { return std::nullopt; }

    const Instrument* instrument(InstrumentId) const noexcept override { return &listed; }

    const portfolio::Position* position(InstrumentId) const noexcept override { return nullptr; }

    FixedE9 unrealized(InstrumentId, PriceTicks) const noexcept override { return FixedE9::from_raw(0); }

    std::optional<ClientOrderId> set_target(const portfolio::PortfolioTarget&) noexcept override {
        return std::nullopt;
    }

    void set_timer(std::uint32_t, std::uint64_t) noexcept override {}

    const Clock& clock() const noexcept override { return clock_; }

    Instrument listed{btc_perp()};
    std::vector<ClientOrderId> sent;
    std::vector<strategy::NewOrder> orders;
    std::vector<OrderOrigin> origins;
    std::vector<ClientOrderId> cancels;

private:
    LiveClock clock_;
};

constexpr std::uint16_t k_joe = 9;
constexpr std::uint16_t k_book = 1030;
constexpr std::uint64_t k_first_parent = 7'000'000;

struct Executor {
    Market market;
    ContextSink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Desk desk{market, sink, clock, curves, {.first_parent_id = k_first_parent, .own_book = k_joe}};
    Kernel kernel;
    strategy::Factory types;
    strategy::Registry live{{.context_for = [this](std::uint16_t) -> strategy::Context* { return &kernel; },
                             .cancel_working = [](std::uint16_t) -> std::size_t { return 0; },
                             .wall = [] { return UnixNanos{1}; }}};

    Executor() {
        (void)types.add("joe", [this](const strategy::Params&) -> strategy::MakeResult {
            return std::make_unique<Runner>(desk, sink);
        });
        EXPECT_TRUE(live.create({.name = "joe", .type = "joe", .strategy_id = k_joe}, types).has_value());
        EXPECT_TRUE(live.start(k_joe, {}).has_value());
        live.set_executor(k_joe);
    }

    std::uint64_t start(std::uint16_t book, std::string ref = {}) {
        ParentSpec spec{};
        spec.listing.symbol = *SymbolText::from("BTC/USDT");
        spec.qty = d("0.05");
        spec.algo = Algo::Pegged;
        spec.max_child_notional = d("1000");
        spec.book = book;
        spec.external_ref = std::move(ref);
        return *desk.start(spec);
    }
};

TEST(JoeBooks, AChildFillBooksToItsParentsBookAndStillReachesJoe) {
    Executor x;
    const auto parent = x.start(k_book, "rfq-12");
    EXPECT_EQ(parent, k_first_parent);
    ASSERT_EQ(x.kernel.sent.size(), 1U);
    const auto child = x.kernel.sent[0];
    EXPECT_EQ(child.strategy_id(), k_book);
    EXPECT_TRUE(child.is_child());
    EXPECT_EQ(x.kernel.origins[0].parent, parent);
    EXPECT_EQ(x.kernel.origins[0].external_ref.view(), "rfq-12");

    OrderEvent fill{};
    fill.type = OrderEventType::Filled;
    fill.instrument = btc_on_binance();
    fill.side = Side::Buy;
    fill.client_order_id = child;
    fill.qty = QtyLots::from_raw(20);
    fill.leaves_qty = QtyLots::from_raw(30);
    fill.price = PriceTicks::from_raw(9998);
    fill.fee = d("0");
    fill.exec_id = *ExecId::from("e-1");

    // the book strategyd keeps: keyed on the id, so the fill lands on the parent's book and nowhere else
    portfolio::PositionBook positions;
    ASSERT_TRUE(positions.on_fill(x.kernel.listed, fill, fill.qty).has_value());
    const auto* held = positions.find({k_book, btc_on_binance()});
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->qty().raw, 20);
    EXPECT_EQ(positions.find({k_joe, btc_on_binance()}), nullptr);

    x.live.on_order_event(fill);
    const auto* working = x.desk.find(parent);
    ASSERT_NE(working, nullptr);
    EXPECT_EQ(working->filled().raw, d("0.02").raw);
    EXPECT_EQ(working->spec().book, k_book);
}

TEST(JoeBooks, ACancelOfAChildOnAnotherBookGoesThroughTheExecutor) {
    Executor x;
    const auto parent = x.start(k_book);
    ASSERT_TRUE(x.desk.cancel(parent).has_value());
    ASSERT_EQ(x.kernel.cancels.size(), 1U);
    EXPECT_EQ(x.kernel.cancels[0], x.kernel.sent[0]);
}

TEST(JoeBooks, AParentThatNamesNoBookIsJoesOwnAndParentIdsDoNotRepeatAfterARestart) {
    Executor x;
    const auto first = x.start(0);
    EXPECT_EQ(x.desk.find(first)->spec().book, k_joe);
    EXPECT_EQ(x.kernel.sent[0].strategy_id(), k_joe);

    const auto before = parent_id_seed(UnixNanos{1'000'000 * k_s});
    const auto after = parent_id_seed(UnixNanos{1'000'000 * k_s + 1'000'000});
    EXPECT_GE(after, before + 4096) << "a restart a millisecond later starts past 4096 parents";
}

TEST(JoeBooks, AnExternalRefLongerThanTheJournalHoldsIsRefused) {
    Executor x;
    ParentSpec spec{};
    spec.listing.symbol = *SymbolText::from("BTC/USDT");
    spec.qty = d("0.05");
    spec.algo = Algo::Pegged;
    spec.max_child_notional = d("1000");
    spec.external_ref = std::string(41, 'x');
    const auto refused = x.desk.start(spec);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().error, DeskError::BadSpec);
}

TEST(JoeRoutesBooks, APostResolvesItsBookByName) {
    Executor x;
    books::MemoryBookStore store;
    books::Registry names{store, [] { return UnixNanos{1}; }};
    ASSERT_TRUE(names.load().has_value());
    const JoeRoutes routes{&x.desk, &names};
    const std::string
        body = R"({"symbol":"BTC/USDT","side":"buy","qty":"0.05","algo":"pegged","max_child_notional":"1000")";
    const auto post = [&](std::string extra, std::string_view query = {}) {
        const auto text = body + extra + "}";
        return *routes({.method = "POST", .path = "/joe/parents", .query = query, .body = text});
    };

    const auto hedged = post(R"(,"book":"hedge","external_ref":"rfq-3")");
    EXPECT_EQ(hedged.status, 201) << hedged.body;
    const auto hedge_id = names.find("hedge")->id;
    EXPECT_EQ(x.kernel.sent.back().strategy_id(), hedge_id);
    EXPECT_EQ(x.kernel.origins.back().external_ref.view(), "rfq-3");

    // admind names the operator; the body's book wins over it
    EXPECT_EQ(post("", "default_book=manual:vova").status, 201);
    EXPECT_EQ(x.kernel.sent.back().strategy_id(), names.find("manual:vova")->id);
    EXPECT_EQ(post(R"(,"book":"hedge")", "default_book=manual:vova").status, 201);
    EXPECT_EQ(x.kernel.sent.back().strategy_id(), hedge_id);

    EXPECT_EQ(post("").status, 201);
    EXPECT_EQ(x.kernel.sent.back().strategy_id(), k_joe);

    EXPECT_EQ(post(R"(,"book":"client:nobody")").status, 404) << "a client's book is opened by its own path";

    const auto list = *routes({.method = "GET", .path = "/joe/parents", .query = "book=hedge"});
    EXPECT_NE(list.body.find(R"("book":"hedge")"), std::string::npos) << list.body;
    EXPECT_NE(list.body.find(R"("book_kind":"hedge")"), std::string::npos) << list.body;
    EXPECT_NE(list.body.find(R"("external_ref":"rfq-3")"), std::string::npos) << list.body;
    EXPECT_EQ(list.body.find("manual:vova"), std::string::npos) << list.body;
}

} // namespace
} // namespace raikiri::joe
