#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/parent_journal.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace raikiri::joe {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;
constexpr std::uint64_t k_tick = 250'000'000ULL;

FixedE9 d(std::string_view text) {
    return *parse_decimal_e9(text);
}

InstrumentId on(VenueId id) {
    return InstrumentId::make(id, MarketType::Perp, SymbolId{7});
}

Listing btc() {
    Listing out{};
    out.symbol = *SymbolText::from("BTC/USDT");
    return out;
}

struct Touch {
    FixedE9 bid;
    FixedE9 ask;
    bool fresh{true};
};

class Market final : public MarketView {
public:
    std::vector<VenueBook> books(const Listing&, Side side) const override {
        std::vector<VenueBook> out;
        for (const auto& [venue, touch] : touches) {
            if (touch.fresh) {
                out.push_back({.instrument = on(venue),
                               .lot = d("0.001"),
                               .levels = {{side == Side::Buy ? touch.ask : touch.bid, d("1")}}});
            }
        }
        return out;
    }

    std::optional<Scale> scale(InstrumentId) const override {
        return Scale{.listing = btc(), .tick = d("0.01"), .lot = d("0.001")};
    }

    std::map<VenueId, Touch> touches{{VenueId::Binance, {.bid = d("99.98"), .ask = d("100")}}};
};

// the journal and the venue write to one list, so a test reads which came first
class Sink final : public OrderSink {
public:
    explicit Sink(std::vector<std::string>& events) : events_{&events} {}

    bool running() const noexcept override { return true; }

    std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept override {
        sent.push_back(order);
        events_->push_back("submit:" + std::to_string(sent.size()));
        return sent.size();
    }

    void cancel(std::uint64_t id) noexcept override {
        cancels.push_back(id);
        events_->push_back("cancel:" + std::to_string(id));
    }

    std::vector<ChildOrder> sent;
    std::vector<std::uint64_t> cancels;

private:
    std::vector<std::string>* events_;
};

class Log final : public ParentLog {
public:
    explicit Log(std::vector<std::string>* events = nullptr) : events_{events} {}

    void record(const ParentRecord& record) noexcept override {
        rows.push_back(encode_parent_record(record));
        if (events_ != nullptr) {
            events_->push_back("record:" + std::to_string(static_cast<int>(record.kind)) + ":" +
                               std::to_string(record.parent));
        }
    }

    std::vector<std::string> rows;

private:
    std::vector<std::string>* events_;
};

class Positions final : public PositionView {
public:
    FixedE9 position(std::uint16_t, InstrumentId instrument) const noexcept override {
        const auto it = held.find(instrument.packed);
        return it == held.end() ? FixedE9::from_raw(0) : it->second;
    }

    std::map<std::uint64_t, FixedE9> held;
};

struct Fixture {
    Fixture() { desk.set_positions(&positions); }

    std::vector<std::string> events;
    Market market;
    Sink sink{events};
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Positions positions;
    Desk desk{market, sink, clock, curves};

    void quote(std::string_view bid, std::string_view ask) {
        market.touches.at(VenueId::Binance) = {.bid = d(bid), .ask = d(ask)};
    }

    void hold(std::string_view qty) { positions.held[on(VenueId::Binance).packed] = d(qty); }

    void pass(std::uint64_t ns = k_tick) {
        clock.advance(ns);
        desk.tick();
    }

    // books the position first, as strategyd does before the event reaches joe
    void fill(std::uint64_t id, std::int64_t lots, std::string_view px) {
        const auto qty = d("0.001").scaled_by(lots);
        auto& held = positions.held.try_emplace(on(VenueId::Binance).packed, FixedE9::from_raw(0)).first->second;
        held = held + (sink.sent.at(id - 1).side == Side::Buy ? qty : FixedE9::from_raw(-qty.raw));
        desk.on_fill(id,
                     on(VenueId::Binance),
                     QtyLots::from_raw(lots),
                     PriceTicks::from_raw(d(px).raw / d("0.01").raw),
                     d("0.001"));
    }

    [[nodiscard]] const Parent& parent(std::uint64_t id) const { return *desk.find(id); }

    void restart_from(const Log& log, std::optional<ParentRecord::Kind> lost = std::nullopt) {
        for (const auto& row : log.rows) {
            const auto record = decode_parent_record(row);
            ASSERT_TRUE(record.has_value());
            if (!lost.has_value() || record->kind != *lost) {
                desk.restore(*record);
            }
        }
        desk.finish_restore();
        (void)desk.resume_restored();
    }
};

ParentSpec entry_spec(std::string_view take_profit, std::string_view stop_loss) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.side = Side::Buy;
    spec.qty = d("0.01");
    spec.algo = Algo::Sor;
    spec.max_child_notional = d("1000");
    spec.attach = {.take_profit = d(take_profit), .stop_loss = d(stop_loss)};
    return spec;
}

ParentSpec stop_spec(std::string_view level, std::string oco) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.side = Side::Sell;
    spec.qty = d("0.01");
    spec.algo = Algo::Stop;
    spec.trigger.price = d(level);
    spec.max_child_notional = d("1000");
    spec.oco = std::move(oco);
    return spec;
}

// a sell resting at 110 with a child out from the start
ParentSpec resting_spec(std::string oco) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.side = Side::Sell;
    spec.qty = d("0.01");
    spec.limit = d("110");
    spec.algo = Algo::Iceberg;
    spec.iceberg.show = d("0.01");
    spec.venue = VenueId::Binance;
    spec.max_child_notional = d("2000");
    spec.oco = std::move(oco);
    return spec;
}

std::ptrdiff_t at(const std::vector<std::string>& events, std::string_view what) {
    const auto it = std::ranges::find(events, what);
    return it == events.end() ? -1 : it - events.begin();
}

TEST(JoeBracket, TheFirstFillMakesBothLegsSizedToItAndMoreFillsGrowThem) {
    Fixture f;
    const auto entry = *f.desk.start(entry_spec("110", "90"));
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.desk.parents().size(), 1U);

    f.fill(1, 4, "100");
    ASSERT_EQ(f.desk.parents().size(), 3U);
    const auto& take = f.parent(entry + 1);
    const auto& stop = f.parent(entry + 2);
    EXPECT_EQ(take.spec().algo, Algo::TakeProfit);
    EXPECT_EQ(stop.spec().algo, Algo::Stop);
    for (const auto* leg : {&take, &stop}) {
        EXPECT_EQ(leg->spec().side, Side::Sell);
        EXPECT_TRUE(leg->spec().reduce_only);
        EXPECT_EQ(leg->spec().oco, "bracket:" + std::to_string(entry));
        EXPECT_EQ(leg->spec().bracket_of, entry);
        EXPECT_EQ(leg->spec().qty, d("0.004"));
    }
    EXPECT_EQ(take.spec().trigger.price, d("110"));
    EXPECT_EQ(stop.spec().trigger.price, d("90"));

    f.pass();
    EXPECT_EQ(take.status(), Status::Working);
    EXPECT_EQ(take.qty(), d("0.004"));
    f.fill(1, 3, "100");
    EXPECT_EQ(take.qty(), d("0.007"));
    EXPECT_EQ(stop.qty(), d("0.007"));
    EXPECT_EQ(f.desk.parents().size(), 3U);
    EXPECT_EQ(f.sink.sent.size(), 1U) << "neither leg has fired";
}

TEST(JoeBracket, CancellingTheEntryBeforeAnyFillMakesNothing) {
    Fixture f;
    const auto entry = *f.desk.start(entry_spec("110", "90"));
    ASSERT_TRUE(f.desk.cancel(entry).has_value());
    f.desk.on_closed(1, ChildEnd::Cancelled);
    f.pass();
    EXPECT_EQ(f.desk.parents().size(), 1U);
}

TEST(JoeBracket, CancellingTheEntryAfterAFillKeepsTheBracketForThatPart) {
    Fixture f;
    const auto entry = *f.desk.start(entry_spec("110", "90"));
    f.fill(1, 4, "100");
    ASSERT_TRUE(f.desk.cancel(entry).has_value());
    f.desk.on_closed(1, ChildEnd::Cancelled);
    f.pass();
    ASSERT_EQ(f.desk.parents().size(), 3U);
    EXPECT_EQ(f.parent(entry + 1).status(), Status::Working);
    EXPECT_EQ(f.parent(entry + 2).status(), Status::Working);
    EXPECT_EQ(f.parent(entry + 2).qty(), d("0.004"));

    f.quote("89", "89.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent.back().side, Side::Sell);
    EXPECT_EQ(f.sink.sent.back().lots, 4);
    EXPECT_TRUE(f.sink.sent.back().reduce_only);
    EXPECT_EQ(f.parent(entry + 1).status(), Status::Cancelled);
    EXPECT_TRUE(f.parent(entry + 1).reason().starts_with("oco: parent " + std::to_string(entry + 2)));
}

TEST(JoeBracket, ALegNeverSendsMoreThanTheOpenPosition) {
    Fixture f;
    const auto entry = *f.desk.start(entry_spec("110", "90"));
    f.fill(1, 7, "100");
    f.pass();
    // something else sold most of it
    f.hold("0.002");
    f.quote("89", "89.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent.back().lots, 2);
    EXPECT_EQ(f.parent(entry + 2).qty(), d("0.007"));
}

TEST(JoeBracket, BothLegsPastTheirLevelsInOneTickSendOnlyTheFirst) {
    Fixture f;
    // a take-profit at 99 and a stop at 100.5 both fire at the 99.99 mid
    const auto entry = *f.desk.start(entry_spec("99", "100.5"));
    f.fill(1, 4, "100");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent.back().lots, 4);
    EXPECT_NE(f.parent(entry + 1).trigger().fired.ns, 0U);
    EXPECT_EQ(f.parent(entry + 2).status(), Status::Cancelled);
    EXPECT_EQ(f.parent(entry + 2).trigger().fired.ns, 0U);
    f.pass(2 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 2U);
}

TEST(JoeBracket, AStaleBookFiresNoLeg) {
    Fixture f;
    const auto entry = *f.desk.start(entry_spec("110", "90"));
    f.fill(1, 4, "100");
    f.pass();
    f.quote("80", "80.02");
    f.market.touches.at(VenueId::Binance).fresh = false;
    f.pass();
    f.pass();
    EXPECT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(entry + 2).trigger().fired.ns, 0U);
    f.market.touches.at(VenueId::Binance).fresh = true;
    f.pass();
    EXPECT_EQ(f.sink.sent.size(), 2U);
}

TEST(JoeOco, TwoStopsFiringInOneTickSendOnlyTheFirst) {
    Fixture f;
    const auto one = *f.desk.start(stop_spec("99.5", "g"));
    const auto two = *f.desk.start(stop_spec("99.4", "g"));
    const auto other = *f.desk.start(stop_spec("99.4", "h"));
    f.quote("99", "99.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 2U) << "one of g and the one in h";
    EXPECT_NE(f.parent(one).trigger().fired.ns, 0U);
    EXPECT_EQ(f.parent(two).status(), Status::Cancelled);
    EXPECT_NE(f.parent(other).trigger().fired.ns, 0U);
}

TEST(JoeOco, ASiblingCancelIsJournaledBeforeItIsSent) {
    Fixture f;
    Log log{&f.events};
    f.desk.set_log(&log);
    const auto resting = *f.desk.start(resting_spec("g"));
    const auto stop = *f.desk.start(stop_spec("99.5", "g"));
    ASSERT_EQ(f.sink.sent.size(), 1U);
    f.quote("99", "99.02");
    f.pass();
    EXPECT_EQ(f.parent(resting).status(), Status::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    const auto recorded = at(f.events, "record:3:" + std::to_string(resting));
    const auto cancelled = at(f.events, "cancel:1");
    const auto sent = at(f.events, "submit:2");
    ASSERT_GE(recorded, 0);
    EXPECT_LT(recorded, cancelled);
    EXPECT_LT(cancelled, sent) << "the sibling's cancel goes before the winner's child";
    EXPECT_NE(f.parent(stop).trigger().fired.ns, 0U);
}

TEST(JoeOco, AFillOnOneMemberCancelsTheOthers) {
    Fixture f;
    const auto resting = *f.desk.start(resting_spec("g"));
    const auto stop = *f.desk.start(stop_spec("99.5", "g"));
    f.fill(1, 2, "110");
    EXPECT_EQ(f.parent(stop).status(), Status::Cancelled);
    EXPECT_EQ(f.parent(resting).status(), Status::Working);
    f.quote("99", "99.02");
    f.pass();
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoeOcoRestart, TheGroupComesBackAndNothingFiresTwice) {
    Log log;
    std::uint64_t entry = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        entry = *before.desk.start(entry_spec("110", "90"));
        before.fill(1, 4, "100");
        before.pass();
        before.quote("89", "89.02");
        before.pass();
        ASSERT_EQ(before.sink.sent.size(), 2U);
    }
    Fixture after;
    after.hold("0.004");
    after.quote("89", "89.02");
    after.restart_from(log);
    ASSERT_EQ(after.desk.parents().size(), 3U);
    EXPECT_EQ(after.parent(entry + 1).status(), Status::Cancelled);
    EXPECT_NE(after.parent(entry + 2).trigger().fired.ns, 0U);
    EXPECT_TRUE(after.parent(entry + 2).spec().reduce_only);
    EXPECT_EQ(after.parent(entry + 2).qty(), d("0.004"));
    after.pass();
    after.quote("111", "111.02");
    after.pass(2 * k_s);
    EXPECT_TRUE(after.sink.sent.empty());
    EXPECT_EQ(after.desk.parents().size(), 3U);
}

TEST(JoeOcoRestart, ACancelJournaledButNeverSentGoesAfterTheRestart) {
    Log log;
    std::uint64_t resting = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        resting = *before.desk.start(resting_spec("g"));
        (void)*before.desk.start(stop_spec("99.5", "g"));
        before.quote("99", "99.02");
        before.pass();
        ASSERT_EQ(before.sink.cancels.size(), 1U);
    }
    Fixture after;
    after.quote("99", "99.02");
    after.restart_from(log);
    EXPECT_EQ(after.parent(resting).status(), Status::Cancelled);
    ASSERT_EQ(after.sink.cancels.size(), 1U);
    EXPECT_EQ(after.sink.cancels.front(), 1U);
    after.pass(2 * k_s);
    EXPECT_TRUE(after.sink.sent.empty());
}

TEST(JoeOcoRestart, AWinnerWhoseSiblingCancelWasLostStillCancelsIt) {
    Log log;
    std::uint64_t resting = 0;
    std::uint64_t stop = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        resting = *before.desk.start(resting_spec("g"));
        stop = *before.desk.start(stop_spec("99.5", "g"));
        before.quote("99", "99.02");
        before.pass();
    }
    Fixture after;
    Log again;
    after.desk.set_log(&again);
    after.quote("99", "99.02");
    after.restart_from(log, ParentRecord::Kind::StatusChanged);
    EXPECT_EQ(after.parent(resting).status(), Status::Cancelled);
    EXPECT_NE(after.parent(stop).status(), Status::Cancelled);
    ASSERT_EQ(after.sink.cancels.size(), 1U);
    EXPECT_FALSE(again.rows.empty());
    after.pass(2 * k_s);
    EXPECT_TRUE(after.sink.sent.empty());
}

} // namespace
} // namespace raikiri::joe
