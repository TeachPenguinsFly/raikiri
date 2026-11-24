#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/parent_journal.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/trigger.hpp>

#include <gtest/gtest.h>

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

class Sink final : public OrderSink {
public:
    bool running() const noexcept override { return true; }

    std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept override {
        sent.push_back(order);
        return sent.size();
    }

    void cancel(std::uint64_t id) noexcept override { cancels.push_back(id); }

    std::vector<ChildOrder> sent;
    std::vector<std::uint64_t> cancels;
};

// what a journal would hold, through its own encoding
class Log final : public ParentLog {
public:
    void record(const ParentRecord& record) noexcept override { rows.push_back(encode_parent_record(record)); }

    std::vector<std::string> rows;
};

struct Fixture {
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Desk desk{market, sink, clock, curves};

    Touch& book() { return market.touches.at(VenueId::Binance); }

    void quote(std::string_view bid, std::string_view ask) {
        book().bid = d(bid);
        book().ask = d(ask);
    }

    void pass(std::uint64_t ns = k_tick) {
        clock.advance(ns);
        desk.tick();
    }

    void fill(std::uint64_t id, std::int64_t lots, std::string_view px) {
        desk.on_fill(id,
                     on(VenueId::Binance),
                     QtyLots::from_raw(lots),
                     PriceTicks::from_raw(d(px).raw / d("0.01").raw),
                     d("0.001"));
    }

    void trade(std::string_view px, VenueId venue = VenueId::Binance, UnixNanos at = {}) {
        desk.on_trade(on(venue), PriceTicks::from_raw(d(px).raw / d("0.01").raw), QtyLots::from_raw(1), at);
    }

    [[nodiscard]] const Parent& parent(std::uint64_t id) const { return *desk.find(id); }

    // a second process: the first one's journal, then the venue agreeing with the book
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

ParentSpec trigger_spec(Algo algo, Side side, std::string_view price) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.side = side;
    spec.qty = d("0.01");
    spec.algo = algo;
    spec.trigger.price = d(price);
    spec.max_child_notional = d("1000");
    return spec;
}

ParentSpec trailing_spec(Side side, std::uint32_t callback_bp) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.side = side;
    spec.qty = d("0.01");
    spec.algo = Algo::TrailingStop;
    spec.trigger.callback_bp = callback_bp;
    spec.max_child_notional = d("1000");
    return spec;
}

// whether a trigger at `level` has a child out after the book moves from 99.98/100 to bid/ask
bool fires(Algo algo, Side side, std::string_view level, std::string_view bid, std::string_view ask) {
    Fixture f;
    const auto id = f.desk.start(trigger_spec(algo, side, level));
    EXPECT_TRUE(id.has_value());
    EXPECT_TRUE(f.sink.sent.empty());
    f.quote(bid, ask);
    f.pass();
    return !f.sink.sent.empty() && f.sink.sent.front().side == side;
}

VenueBook one_level(VenueId venue, std::string_view price) {
    return {.instrument = on(venue), .lot = d("0.001"), .levels = {{d(price), d("1")}}};
}

TEST(JoeTrigger, AStopFiresWithTheMoveAgainstItAndATakeProfitWithTheMoveForIt) {
    EXPECT_TRUE(fires(Algo::Stop, Side::Sell, "99.5", "99", "99.02"));
    EXPECT_FALSE(fires(Algo::Stop, Side::Sell, "99.5", "100.6", "100.62"));
    EXPECT_TRUE(fires(Algo::Stop, Side::Buy, "100.5", "100.6", "100.62"));
    EXPECT_FALSE(fires(Algo::Stop, Side::Buy, "100.5", "99", "99.02"));

    EXPECT_TRUE(fires(Algo::TakeProfit, Side::Sell, "100.5", "100.6", "100.62"));
    EXPECT_FALSE(fires(Algo::TakeProfit, Side::Sell, "100.5", "99", "99.02"));
    EXPECT_TRUE(fires(Algo::TakeProfit, Side::Buy, "99.5", "99", "99.02"));
    EXPECT_FALSE(fires(Algo::TakeProfit, Side::Buy, "99.5", "100.6", "100.62"));
}

TEST(JoeTrigger, TheLevelItselfFiresAndOneTickShortOfItDoesNot) {
    const auto stop = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    TriggerState state{};
    EXPECT_FALSE(observe(stop, state, d("99.51"), UnixNanos{5}));
    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.fired.ns, 0U);
    EXPECT_TRUE(observe(stop, state, d("99.5"), UnixNanos{6}));
    EXPECT_EQ(state.fired.ns, 6U);
    EXPECT_EQ(state.price, d("99.5"));

    const auto buy = trigger_spec(Algo::Stop, Side::Buy, "100.5");
    TriggerState other{};
    EXPECT_FALSE(observe(buy, other, d("100.49"), UnixNanos{5}));
    EXPECT_TRUE(observe(buy, other, d("100.5"), UnixNanos{6}));
}

TEST(JoeTrigger, AFiredTriggerTakesNoMorePricesAndNoPriceIsNotAPrice) {
    const auto stop = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    TriggerState state{};
    EXPECT_FALSE(observe(stop, state, FixedE9::undef(), UnixNanos{5}));
    EXPECT_FALSE(observe(stop, state, FixedE9::from_raw(0), UnixNanos{5}));
    EXPECT_FALSE(state.active);
    EXPECT_TRUE(observe(stop, state, d("90"), UnixNanos{6}));
    EXPECT_FALSE(observe(stop, state, d("80"), UnixNanos{7}));
    EXPECT_EQ(state.fired.ns, 6U);
    EXPECT_EQ(state.price, d("90"));
}

TEST(JoeTrigger, ATrailingSellFollowsTheHighAndNeverComesBackDown) {
    const auto spec = trailing_spec(Side::Sell, 100);
    TriggerState state{};
    EXPECT_TRUE(trigger_level(spec, state).is_undef());
    EXPECT_FALSE(observe(spec, state, d("100"), UnixNanos{1}));
    EXPECT_EQ(trigger_level(spec, state), d("99"));
    EXPECT_FALSE(observe(spec, state, d("110"), UnixNanos{2}));
    EXPECT_EQ(state.mark, d("110"));
    EXPECT_EQ(trigger_level(spec, state), d("108.9"));
    EXPECT_FALSE(observe(spec, state, d("109"), UnixNanos{3}));
    EXPECT_EQ(state.mark, d("110"));
    EXPECT_EQ(trigger_level(spec, state), d("108.9"));
    EXPECT_TRUE(observe(spec, state, d("108.9"), UnixNanos{4}));
    EXPECT_EQ(state.fired.ns, 4U);
}

TEST(JoeTrigger, ATrailingBuyFollowsTheLow) {
    const auto spec = trailing_spec(Side::Buy, 100);
    TriggerState state{};
    EXPECT_FALSE(observe(spec, state, d("100"), UnixNanos{1}));
    EXPECT_FALSE(observe(spec, state, d("90"), UnixNanos{2}));
    EXPECT_FALSE(observe(spec, state, d("90.8"), UnixNanos{3}));
    EXPECT_EQ(state.mark, d("90"));
    EXPECT_EQ(trigger_level(spec, state), d("90.9"));
    EXPECT_TRUE(observe(spec, state, d("91"), UnixNanos{4}));
}

TEST(JoeTrigger, ATrailingStopWaitsForItsActivationAndTrailsFromThere) {
    auto sell = trailing_spec(Side::Sell, 100);
    sell.trigger.activation = d("105");
    TriggerState state{};
    EXPECT_FALSE(observe(sell, state, d("100"), UnixNanos{1}));
    // a fall before the activation is not this order's business
    EXPECT_FALSE(observe(sell, state, d("80"), UnixNanos{2}));
    EXPECT_FALSE(state.active);
    EXPECT_TRUE(trigger_level(sell, state).is_undef());
    EXPECT_FALSE(observe(sell, state, d("106"), UnixNanos{3}));
    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.mark, d("106"));
    EXPECT_FALSE(observe(sell, state, d("104.95"), UnixNanos{4}));
    EXPECT_TRUE(observe(sell, state, d("104.94"), UnixNanos{5}));

    auto buy = trailing_spec(Side::Buy, 100);
    buy.trigger.activation = d("95");
    TriggerState low{};
    EXPECT_FALSE(observe(buy, low, d("120"), UnixNanos{1}));
    EXPECT_FALSE(low.active);
    EXPECT_FALSE(observe(buy, low, d("95"), UnixNanos{2}));
    EXPECT_TRUE(low.active);
    EXPECT_TRUE(observe(buy, low, d("95.95"), UnixNanos{3}));
}

TEST(JoeTrigger, TheMidLeavesOutAVenueThatIsOneSidedOrCrossedAndIsNothingWithoutAny) {
    const std::vector<VenueBook> bids{one_level(VenueId::Binance, "99"), one_level(VenueId::Okx, "101")};
    const std::vector<VenueBook> asks{one_level(VenueId::Binance, "100"), one_level(VenueId::Okx, "100.5")};
    // okx is crossed, so only binance speaks
    EXPECT_EQ(trigger_mid(bids, asks, VenueId::Invalid), d("99.5"));
    EXPECT_TRUE(trigger_mid(bids, asks, VenueId::Okx).is_undef());

    const std::vector<VenueBook> no_bids{};
    EXPECT_TRUE(trigger_mid(no_bids, asks, VenueId::Invalid).is_undef());
    EXPECT_TRUE(trigger_mid(bids, no_bids, VenueId::Invalid).is_undef());

    const std::vector<VenueBook> both_bids{one_level(VenueId::Binance, "99"), one_level(VenueId::Okx, "99.2")};
    const std::vector<VenueBook> both_asks{one_level(VenueId::Binance, "100"), one_level(VenueId::Okx, "99.6")};
    EXPECT_EQ(trigger_mid(both_bids, both_asks, VenueId::Invalid), d("99.4"));
    EXPECT_EQ(trigger_mid(both_bids, both_asks, VenueId::Binance), d("99.5"));
}

TEST(JoeTrigger, ValidateRefusesATriggerThatDoesNotAddUp) {
    EXPECT_TRUE(validate(trigger_spec(Algo::Stop, Side::Sell, "99.5")).has_value());
    EXPECT_TRUE(validate(trailing_spec(Side::Sell, 100)).has_value());

    auto spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.trigger.price = FixedE9::undef();
    EXPECT_EQ(validate(spec).error(), SpecError::NoTrigger);
    spec = trigger_spec(Algo::TakeProfit, Side::Sell, "99.5");
    spec.trigger.callback_bp = 100;
    EXPECT_EQ(validate(spec).error(), SpecError::BadTrigger);
    spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.horizon_ns = 1;
    EXPECT_EQ(validate(spec).error(), SpecError::BadTriggerExpiry);

    spec = trailing_spec(Side::Sell, 0);
    EXPECT_EQ(validate(spec).error(), SpecError::NoCallback);
    spec = trailing_spec(Side::Sell, 5001);
    EXPECT_EQ(validate(spec).error(), SpecError::NoCallback);
    spec = trailing_spec(Side::Sell, 100);
    spec.trigger.price = d("99");
    EXPECT_EQ(validate(spec).error(), SpecError::BadTrigger);
    spec = trailing_spec(Side::Sell, 100);
    spec.trigger.activation = FixedE9::from_raw(0);
    EXPECT_EQ(validate(spec).error(), SpecError::BadActivation);
}

TEST(JoeStop, SendsNothingUntilTheMidReachesTheLevelThenTakesTheBook) {
    Fixture f;
    const auto id = *f.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
    f.pass();
    f.quote("99.6", "99.62");
    f.pass();
    EXPECT_TRUE(f.sink.sent.empty());
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    EXPECT_TRUE(f.parent(id).trigger().active);
    EXPECT_EQ(f.parent(id).trigger().fired.ns, 0U);

    f.quote("99.48", "99.52");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    const auto& child = f.sink.sent.front();
    EXPECT_EQ(child.side, Side::Sell);
    EXPECT_EQ(child.tif, ChildTif::Ioc);
    EXPECT_EQ(child.lots, 10);
    EXPECT_EQ(child.limit, d("99.48"));
    EXPECT_EQ(f.parent(id).trigger().fired.ns, f.clock.wall().ns);
    EXPECT_EQ(f.parent(id).trigger().price, d("99.5"));
    EXPECT_EQ(f.parent(id).arrival(), d("99.5"));
}

TEST(JoeStop, APriceAlreadyPastTheLevelFiresOnTheFirstOneSeen) {
    Fixture f;
    const auto id = *f.desk.start(trigger_spec(Algo::Stop, Side::Sell, "101"));
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).trigger().price, d("99.99"));

    Fixture gap;
    const auto other = *gap.desk.start(trigger_spec(Algo::Stop, Side::Buy, "100.5"));
    gap.quote("140", "140.02");
    gap.pass();
    ASSERT_EQ(gap.sink.sent.size(), 1U);
    EXPECT_EQ(gap.sink.sent.front().limit, d("140.02"));
    EXPECT_EQ(gap.parent(other).trigger().price, d("140.01"));
}

TEST(JoeStop, FiresOnceHoweverOftenThePriceComesBack) {
    Fixture f;
    const auto id = *f.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
    f.quote("99", "99.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    const auto fired = f.parent(id).trigger().fired;

    // the child is still out: the level is crossed again both ways and nothing joins it
    for (int i = 0; i < 4; ++i) {
        f.quote("100.6", "100.62");
        f.pass(2 * k_s);
        f.quote("99", "99.02");
        f.pass(2 * k_s);
    }
    EXPECT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).trigger().fired.ns, fired.ns);

    f.fill(1, 10, "99");
    f.pass();
    EXPECT_EQ(f.parent(id).status(), Status::Done);
    f.quote("100.6", "100.62");
    f.pass(2 * k_s);
    f.quote("99", "99.02");
    f.pass(2 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoeStop, AStaleOrCrossedBookFiresNothingAndTheFirstLiveOneDoes) {
    Fixture f;
    const auto id = *f.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
    f.quote("90", "90.02");
    f.book().fresh = false;
    f.pass();
    f.pass(10 * k_s);
    EXPECT_TRUE(f.sink.sent.empty());
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Stale);
    EXPECT_EQ(f.parent(id).trigger().fired.ns, 0U);

    f.book().fresh = true;
    f.quote("90.02", "90");
    f.pass();
    EXPECT_TRUE(f.sink.sent.empty());
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Stale);

    f.quote("90", "90.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).slot().hold, Hold::None);
    EXPECT_EQ(f.parent(id).trigger().price, d("90.01"));
}

TEST(JoeStop, AChosenVenueIsTheOnlyPriceReadAndTheOnlyPlaceSent) {
    Fixture all;
    all.market.touches[VenueId::Okx] = {.bid = d("98"), .ask = d("98.02")};
    (void)*all.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
    // the best bid and the best ask of the two halve to 99
    ASSERT_EQ(all.sink.sent.size(), 1U);
    EXPECT_EQ(all.sink.sent.front().instrument, on(VenueId::Binance));

    Fixture one;
    one.market.touches[VenueId::Okx] = {.bid = d("98"), .ask = d("98.02")};
    auto spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.venue = VenueId::Binance;
    (void)*one.desk.start(spec);
    one.pass();
    EXPECT_TRUE(one.sink.sent.empty());
    one.market.touches[VenueId::Okx] = {.bid = d("120"), .ask = d("120.02")};
    one.quote("99.4", "99.42");
    one.pass();
    ASSERT_EQ(one.sink.sent.size(), 1U);
    EXPECT_EQ(one.sink.sent.front().instrument, on(VenueId::Binance));
    EXPECT_EQ(one.sink.sent.front().limit, d("99.4"));
}

TEST(JoeStop, WithALimitRestsThereOnceFired) {
    Fixture f;
    auto spec = trigger_spec(Algo::Stop, Side::Buy, "100.5");
    spec.limit = d("100.8");
    const auto id = *f.desk.start(spec);
    f.pass();
    EXPECT_TRUE(f.sink.sent.empty());

    f.quote("100.6", "100.62");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent.front().side, Side::Buy);
    EXPECT_EQ(f.sink.sent.front().tif, ChildTif::Gtc);
    EXPECT_EQ(f.sink.sent.front().limit, d("100.8"));
    EXPECT_EQ(f.sink.sent.front().lots, 10);

    f.fill(1, 10, "100.62");
    EXPECT_EQ(f.parent(id).status(), Status::Done);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoeStop, OnLastFiresOnATradeAndNeverOnTheBookOrAnOldTrade) {
    Fixture f;
    auto spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.trigger.on = TriggerOn::Last;
    const auto id = *f.desk.start(spec);
    f.quote("99", "99.02");
    f.pass();
    EXPECT_TRUE(f.sink.sent.empty());

    f.trade("99.6");
    EXPECT_TRUE(f.sink.sent.empty());
    EXPECT_TRUE(f.parent(id).trigger().active);
    f.trade("99.4", VenueId::Binance, UnixNanos{f.clock.wall().ns - 6 * k_s});
    EXPECT_TRUE(f.sink.sent.empty());

    f.trade("99.4", VenueId::Binance, UnixNanos{f.clock.wall().ns - 1 * k_s});
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent.front().limit, d("99"));
    EXPECT_EQ(f.parent(id).trigger().price, d("99.4"));
    f.trade("99.3");
    f.pass(2 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoeStop, OnLastWithAVenueHearsOnlyThatVenuesTrades) {
    Fixture f;
    auto spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.trigger.on = TriggerOn::Last;
    spec.venue = VenueId::Binance;
    (void)*f.desk.start(spec);
    f.trade("90", VenueId::Okx);
    EXPECT_TRUE(f.sink.sent.empty());
    f.trade("99.5");
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoeStop, APausedTriggerSeesNothingAndFiresOnTheFirstPriceAfter) {
    Fixture f;
    const auto id = *f.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
    ASSERT_TRUE(f.desk.pause(id).has_value());
    f.quote("95", "95.02");
    f.pass();
    f.pass(5 * k_s);
    EXPECT_TRUE(f.sink.sent.empty());
    ASSERT_TRUE(f.desk.resume(id).has_value());
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).trigger().price, d("95.01"));
}

TEST(JoeStop, TheHorizonEndsAnUnfiredTriggerWithNothingSent) {
    Fixture f;
    auto spec = trigger_spec(Algo::Stop, Side::Sell, "99.5");
    spec.horizon_ns = 10 * k_s;
    const auto id = *f.desk.start(spec);
    f.pass(9 * k_s);
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    f.quote("90", "90.02");
    f.pass(2 * k_s);
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
    EXPECT_NE(f.parent(id).reason().find("trigger"), std::string_view::npos);
    EXPECT_TRUE(f.sink.sent.empty());
}

TEST(JoeTrailingStop, RatchetsWithTheBookAndFiresTheCallbackBelowTheHigh) {
    Fixture f;
    const auto id = *f.desk.start(trailing_spec(Side::Sell, 100));
    EXPECT_EQ(f.parent(id).trigger().mark, d("99.99"));
    f.quote("109.98", "110.02");
    f.pass();
    EXPECT_EQ(f.parent(id).trigger().mark, d("110"));
    f.quote("109", "109.02");
    f.pass();
    EXPECT_EQ(f.parent(id).trigger().mark, d("110"));
    EXPECT_EQ(trigger_level(f.parent(id).spec(), f.parent(id).trigger()), d("108.9"));
    EXPECT_TRUE(f.sink.sent.empty());

    f.quote("108.88", "108.92");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent.front().side, Side::Sell);
    EXPECT_EQ(f.sink.sent.front().limit, d("108.88"));
}

TEST(JoeTrailingStop, ABuyWaitsForItsActivationThenFiresTheCallbackAboveTheLow) {
    Fixture f;
    auto spec = trailing_spec(Side::Buy, 100);
    spec.trigger.activation = d("95");
    const auto id = *f.desk.start(spec);
    f.quote("119.98", "120.02");
    f.pass();
    EXPECT_FALSE(f.parent(id).trigger().active);
    f.quote("89.98", "90.02");
    f.pass();
    EXPECT_TRUE(f.parent(id).trigger().active);
    EXPECT_EQ(f.parent(id).trigger().mark, d("90"));
    f.quote("90.78", "90.82");
    f.pass();
    EXPECT_TRUE(f.sink.sent.empty());
    f.quote("90.98", "91.02");
    f.pass();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent.front().side, Side::Buy);
}

TEST(JoeTriggerRestart, ARecordSurvivesItsEncoding) {
    auto spec = trailing_spec(Side::Sell, 150);
    spec.trigger.on = TriggerOn::Last;
    spec.trigger.activation = d("105");
    const auto created = decode_parent_record(
        encode_parent_record({.kind = ParentRecord::Kind::Created, .parent = 9, .spec = spec}));
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->spec.algo, Algo::TrailingStop);
    EXPECT_EQ(created->spec.trigger.on, TriggerOn::Last);
    EXPECT_EQ(created->spec.trigger.callback_bp, 150U);
    EXPECT_EQ(created->spec.trigger.activation, d("105"));
    EXPECT_TRUE(created->spec.trigger.price.is_undef());

    const TriggerState state{.active = true, .mark = d("110"), .fired = UnixNanos{77}, .price = d("108.9")};
    const auto moved = decode_parent_record(encode_parent_record(
        {.kind = ParentRecord::Kind::Trigger, .parent = 9, .ts = UnixNanos{77}, .trigger = state}));
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->kind, ParentRecord::Kind::Trigger);
    EXPECT_TRUE(moved->trigger.active);
    EXPECT_EQ(moved->trigger.mark, d("110"));
    EXPECT_EQ(moved->trigger.fired.ns, 77U);
    EXPECT_EQ(moved->trigger.price, d("108.9"));
}

TEST(JoeTriggerRestart, ATrailingHighComesBackSoTheStopIsWhereItWas) {
    Log log;
    std::uint64_t id = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        id = *before.desk.start(trailing_spec(Side::Sell, 100));
        before.quote("109.98", "110.02");
        before.pass();
        ASSERT_TRUE(before.sink.sent.empty());
    }
    Fixture after;
    // a book that would not fire a stop trailing from here, and does fire the one trailing from 110
    after.quote("108.48", "108.52");
    after.restart_from(log);
    ASSERT_NE(after.desk.find(id), nullptr);
    EXPECT_TRUE(after.parent(id).trigger().active);
    EXPECT_EQ(after.parent(id).trigger().mark, d("110"));
    EXPECT_EQ(after.parent(id).trigger().fired.ns, 0U);
    EXPECT_TRUE(after.sink.sent.empty());
    after.pass();
    ASSERT_EQ(after.sink.sent.size(), 1U);
    EXPECT_EQ(after.parent(id).trigger().price, d("108.5"));
}

TEST(JoeTriggerRestart, TheJournalFollowsTheMarkInBasisPointSteps) {
    Log log;
    Fixture f;
    f.desk.set_log(&log);
    (void)*f.desk.start(trailing_spec(Side::Sell, 100));
    const auto armed = log.rows.size();
    // less than a basis point above 99.99
    f.quote("99.98", "100.01");
    f.pass();
    EXPECT_EQ(log.rows.size(), armed);
    f.quote("100.98", "101");
    f.pass();
    EXPECT_EQ(log.rows.size(), armed + 1);
}

TEST(JoeTriggerRestart, AFiredTriggerNeitherFiresAgainNorIsForgotten) {
    Log log;
    std::uint64_t id = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        id = *before.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
        before.quote("99", "99.02");
        before.pass();
        ASSERT_EQ(before.sink.sent.size(), 1U);
    }
    {
        // the child is out and the level is crossed again: nothing new goes
        Fixture after;
        after.quote("99", "99.02");
        after.restart_from(log);
        EXPECT_NE(after.parent(id).trigger().fired.ns, 0U);
        EXPECT_EQ(after.parent(id).arrival(), d("99.01"));
        after.pass();
        after.quote("100.6", "100.62");
        after.pass(2 * k_s);
        after.quote("99", "99.02");
        after.pass(2 * k_s);
        EXPECT_TRUE(after.sink.sent.empty());
    }
    {
        // it stopped between the trigger's record and the child: the order still goes, though the price is back
        Fixture after;
        after.restart_from(log, ParentRecord::Kind::ChildSent);
        EXPECT_NE(after.parent(id).trigger().fired.ns, 0U);
        EXPECT_TRUE(after.sink.sent.empty());
        after.pass();
        ASSERT_EQ(after.sink.sent.size(), 1U);
        EXPECT_EQ(after.sink.sent.front().side, Side::Sell);
        after.pass(2 * k_s);
        EXPECT_EQ(after.sink.sent.size(), 1U);
    }
    {
        // the trigger's own record never reached the journal: the child says it fired
        Fixture after;
        after.quote("99", "99.02");
        after.restart_from(log, ParentRecord::Kind::Trigger);
        EXPECT_NE(after.parent(id).trigger().fired.ns, 0U);
        after.pass();
        after.pass(2 * k_s);
        EXPECT_TRUE(after.sink.sent.empty());
    }
}

TEST(JoeTriggerRestart, AnUnfiredStopIsStillArmedAndFiresOnTheFirstPriceAfter) {
    Log log;
    std::uint64_t id = 0;
    {
        Fixture before;
        before.desk.set_log(&log);
        id = *before.desk.start(trigger_spec(Algo::Stop, Side::Sell, "99.5"));
        before.pass();
    }
    Fixture after;
    after.restart_from(log);
    EXPECT_TRUE(after.parent(id).trigger().active);
    after.pass();
    EXPECT_TRUE(after.sink.sent.empty());
    after.quote("99", "99.02");
    after.pass();
    EXPECT_EQ(after.sink.sent.size(), 1U);
}

TEST(JoeTriggerRoutes, TakesEachTriggerAndAnswersItsFields) {
    Fixture f;
    const JoeRoutes routes{&f.desk};
    const auto stop = routes({.method = "POST",
                              .path = "/joe/parents",
                              .body = R"({"symbol":"BTC/USDT","side":"sell","qty":"0.01","algo":"stop",
                                  "venue":"binance","max_child_notional":"1000","trigger":{"price":"99.5"}})"});
    ASSERT_EQ(stop->status, 201) << stop->body;
    const auto trailing = routes({.method = "POST",
                                  .path = "/joe/parents",
                                  .body = R"({"symbol":"BTC/USDT","side":"sell","qty":"0.01","algo":"trailing_stop",
             "trigger":{"source":"last","callback_bp":100,"activation":"105"}})"});
    ASSERT_EQ(trailing->status, 201) << trailing->body;

    const auto one = routes({.method = "GET", .path = "/joe/parents/1"});
    EXPECT_NE(one->body.find(R"("algo":"stop")"), std::string::npos) << one->body;
    EXPECT_NE(one->body.find(R"("trigger":{"price":"99.5","source":"mid","callback_bp":0})"), std::string::npos)
        << one->body;
    EXPECT_NE(one->body.find(R"("trigger_level":"99.5")"), std::string::npos) << one->body;
    EXPECT_EQ(one->body.find(R"("triggered")"), std::string::npos) << one->body;
    const auto two = routes({.method = "GET", .path = "/joe/parents/2"});
    EXPECT_NE(two->body.find(R"("trigger":{"source":"last","callback_bp":100,"activation":"105"})"), std::string::npos)
        << two->body;
    EXPECT_EQ(two->body.find(R"("trigger_level")"), std::string::npos) << two->body;

    f.quote("99", "99.02");
    f.pass();
    const auto fired = routes({.method = "GET", .path = "/joe/parents/1"});
    EXPECT_NE(fired->body.find(R"("triggered":")"), std::string::npos) << fired->body;
    EXPECT_NE(fired->body.find(R"("arrival":"99.01")"), std::string::npos) << fired->body;
}

TEST(JoeTriggerRoutes, RefusesATriggerBodyThatDoesNotAddUp) {
    Fixture f;
    const JoeRoutes routes{&f.desk};
    const auto post = [&](std::string body) {
        return routes({.method = "POST", .path = "/joe/parents", .body = std::move(body)})->status;
    };
    const std::string head = R"({"symbol":"BTC/USDT","side":"sell","qty":"0.01",)";
    EXPECT_EQ(post(head + R"("algo":"stop"})"), 400);
    EXPECT_EQ(post(head + R"("algo":"sor","trigger":{"price":"99.5"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"stop","trigger":{"price":"x"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"stop","trigger":{"price":"99.5","source":"mark"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"stop","post_only":true,"trigger":{"price":"99.5"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"stop","trigger":{}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"take_profit","trigger":{"price":"101","callback_bp":50}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"trailing_stop","trigger":{}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"trailing_stop","trigger":{"price":"99","callback_bp":50}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"take_profit","trigger":{"price":"101"}})"), 201);
    EXPECT_EQ(post(head + R"("algo":"trailing_stop","trigger":{"callback_bp":50}})"), 201);
}

} // namespace
} // namespace raikiri::joe
