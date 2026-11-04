#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/resting.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/tca.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <vector>

namespace raikiri::joe {
namespace {

constexpr std::uint64_t k_s = 1'000'000'000ULL;
constexpr std::uint64_t k_ms = 1'000'000ULL;

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
    bool amend{false};
    bool post_only{true};
    bool fresh{true};
    FixedE9 taker_fee{FixedE9::from_raw(0)};
    FixedE9 depth{d("1")};
};

class Market final : public MarketView {
public:
    std::vector<VenueBook> books(const Listing&, Side side) const override {
        std::vector<VenueBook> out;
        for (const auto& [venue, touch] : touches) {
            if (touch.fresh) {
                out.push_back({.instrument = on(venue),
                               .taker_fee = touch.taker_fee,
                               .lot = d("0.001"),
                               .levels = {{side == Side::Buy ? touch.ask : touch.bid, touch.depth}},
                               .amend = touch.amend,
                               .post_only = touch.post_only});
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
        if (refuse.has_value()) {
            return std::unexpected{*refuse};
        }
        sent.push_back(order);
        return sent.size();
    }

    void cancel(std::uint64_t id) noexcept override { cancels.push_back(id); }

    bool modify(std::uint64_t id, InstrumentId, FixedE9 limit) noexcept override {
        modifies.push_back({id, limit});
        return modify_goes;
    }

    std::vector<ChildOrder> sent;
    std::vector<std::uint64_t> cancels;
    std::vector<std::pair<std::uint64_t, FixedE9>> modifies;
    bool modify_goes{true};
    std::optional<SinkError> refuse;
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

    void pass(std::uint64_t ns) {
        clock.advance(ns);
        desk.tick();
    }

    void fill(std::uint64_t id, std::int64_t lots, std::string_view px = "99.98") {
        desk.on_fill(id,
                     on(VenueId::Binance),
                     QtyLots::from_raw(lots),
                     PriceTicks::from_raw(d(px).raw / d("0.01").raw),
                     d("0.001"));
    }

    [[nodiscard]] const Parent& parent(std::uint64_t id) const { return *desk.find(id); }

    // lots a venue would see as live: sent, unfilled, not yet closed
    [[nodiscard]] std::size_t live_children(std::uint64_t id) const {
        const auto kids = parent(id).children();
        return static_cast<std::size_t>(std::ranges::count_if(kids, &Child::open));
    }
};

ParentSpec iceberg_spec(std::string_view qty = "0.1", std::string_view show = "0.01") {
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d(qty);
    spec.limit = d("100.5");
    spec.algo = Algo::Iceberg;
    spec.iceberg.show = d(show);
    spec.max_child_notional = d("1000");
    return spec;
}

ParentSpec pegged_spec(PegTo to = PegTo::Primary, std::string_view qty = "0.1") {
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d(qty);
    spec.algo = Algo::Pegged;
    spec.peg.to = to;
    spec.peg.min_interval_ns = 1 * k_s;
    spec.max_child_notional = d("1000");
    return spec;
}

TEST(JoeRestingPrice, PegsToEachReferenceWithAnOffsetRoundedAwayFromTheOtherSide) {
    const Quote quote{.state = BookState::Live, .bid = d("99.98"), .ask = d("100.01")};
    const auto tick = d("0.01");
    const auto none = FixedE9::undef();
    PegSpec peg{};
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("99.98"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, none, false), d("100.01"));
    peg.to = PegTo::Market;
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("100.01"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, none, false), d("99.98"));
    peg.to = PegTo::Mid;
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("99.99"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, none, false), d("100"));

    peg.to = PegTo::Primary;
    peg.offset_ticks = 3;
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("99.95"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, none, false), d("100.04"));
    peg.offset_ticks = -1;
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("99.99"));
    peg.offset_ticks = 0;
    peg.offset_bp = 10;
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, none, false), d("99.88"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, none, false), d("100.12"));
}

TEST(JoeRestingPrice, TheLimitCapsAndPostOnlyStopsShortOfTheOtherSide) {
    const Quote quote{.state = BookState::Live, .bid = d("99.98"), .ask = d("100.01")};
    const auto tick = d("0.01");
    PegSpec peg{.to = PegTo::Market};
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, d("100"), false), d("100"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, d("99.99"), false), d("99.99"));
    EXPECT_EQ(peg_price(Side::Buy, peg, quote, tick, FixedE9::undef(), true), d("100"));
    EXPECT_EQ(peg_price(Side::Sell, peg, quote, tick, FixedE9::undef(), true), d("99.99"));
    // a limit off the grid rounds to the side that keeps it
    EXPECT_EQ(rest_price(Side::Buy, d("100.5"), quote, tick, d("99.995"), false), d("99.99"));
    EXPECT_EQ(rest_price(Side::Sell, d("90"), quote, tick, d("100.005"), false), d("100.01"));
}

TEST(JoeRestingPrice, QuoteReadsOneVenueAndCallsAMissingSideStaleAndALockedBookCrossed) {
    const std::vector<VenueBook> bids{{.instrument = on(VenueId::Okx), .levels = {{d("10"), d("1")}}}};
    const std::vector<VenueBook> asks{{.instrument = on(VenueId::Okx), .levels = {{d("11"), d("1")}}},
                                      {.instrument = on(VenueId::Gate), .levels = {{d("9"), d("1")}}}};
    EXPECT_EQ(quote_of(on(VenueId::Okx), bids, asks).state, BookState::Live);
    EXPECT_EQ(quote_of(on(VenueId::Gate), bids, asks).state, BookState::Stale);
    const std::vector<VenueBook> locked{{.instrument = on(VenueId::Okx), .levels = {{d("10"), d("1")}}}};
    EXPECT_EQ(quote_of(on(VenueId::Okx), bids, locked).state, BookState::Crossed);
}

TEST(JoeRestingSor, BestVenueIsTheBestTouchAfterFeesThatCanPost) {
    const std::vector<VenueBook> asks{
        {.instrument = on(VenueId::Binance), .taker_fee = d("0.001"), .levels = {{d("100"), d("1")}}},
        {.instrument = on(VenueId::Okx), .taker_fee = d("0"), .levels = {{d("100.05"), d("1")}}},
        {.instrument = on(VenueId::Gate), .levels = {{d("99"), d("1")}}, .post_only = false}};
    EXPECT_EQ(best_venue(Side::Buy, asks, true)->instrument, on(VenueId::Okx));
    EXPECT_EQ(best_venue(Side::Buy, asks, false)->instrument, on(VenueId::Gate));
    EXPECT_EQ(best_venue(Side::Buy, {}, false), nullptr);
}

TEST(JoeRestingSize, ShowsStayInTheirBandAndNeverExceedWhatIsLeft) {
    IcebergSpec iceberg{.show = d("0.1"), .variance_bp = 2000};
    std::uint64_t random = 42;
    std::vector<std::int64_t> shows;
    for (int i = 0; i < 500; ++i) {
        const auto lots = next_show(iceberg, d("0.001"), 10'000, random);
        EXPECT_GE(lots, 80);
        EXPECT_LE(lots, 120);
        shows.push_back(lots);
    }
    std::ranges::sort(shows);
    EXPECT_NE(shows.front(), shows.back());
    EXPECT_EQ(next_show(iceberg, d("0.001"), 7, random), 7);
    // a show under one lot still sends one
    iceberg = {.show = d("0.0001")};
    EXPECT_EQ(next_show(iceberg, d("0.001"), 10, random), 1);
}

TEST(JoeDepthShare, EachVenueGivesUpItsOwnShareAndEveryChildNamesItsParent) {
    Fixture f;
    f.market.touches = {{VenueId::Binance, {.bid = d("99.98"), .ask = d("100"), .depth = d("10")}},
                        {VenueId::Okx, {.bid = d("99.98"), .ask = d("100"), .depth = d("0.4")}}};
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d("5");
    spec.algo = Algo::Sor;
    spec.max_child_notional = d("1000");
    spec.depth_share_bp = 2500;
    spec.external_ref = "rfq-7";
    const auto id = f.desk.start(spec);
    ASSERT_TRUE(id.has_value());
    f.pass(k_s);
    // a quarter of what each venue holds, not a quarter of the thinnest: 2.5 and 0.1, a lot being 0.001
    ASSERT_GE(f.sink.sent.size(), 2U);
    std::map<std::uint64_t, std::int64_t> lots;
    for (const auto& child : f.sink.sent) {
        // the first batch; a later one sends what this one left
        lots.try_emplace(child.instrument.packed, child.lots);
        EXPECT_EQ(child.origin.parent, *id) << "a child must lead back to the order that made it";
        EXPECT_EQ(child.origin.external_ref.view(), "rfq-7");
    }
    EXPECT_EQ(lots[on(VenueId::Binance).packed], 2500);
    EXPECT_EQ(lots[on(VenueId::Okx).packed], 100);
}

TEST(JoeDepthShare, AThinBookStillGetsOneLotAndOffLeavesTheCapAlone) {
    Fixture f;
    f.book().depth = d("0.001");
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d("0.001");
    spec.algo = Algo::Sor;
    spec.max_child_notional = d("1000");
    spec.depth_share_bp = 2500;
    ASSERT_TRUE(f.desk.start(spec).has_value());
    f.pass(k_s);
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent[0].lots, 1);

    Fixture off;
    off.book().depth = d("10");
    spec.qty = d("5");
    spec.depth_share_bp = 0;
    ASSERT_TRUE(off.desk.start(spec).has_value());
    off.pass(k_s);
    ASSERT_EQ(off.sink.sent.size(), 1U);
    EXPECT_EQ(off.sink.sent[0].lots, 5000);
}

TEST(JoeDepthShare, ABadShareIsRefused) {
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d("1");
    spec.algo = Algo::Sor;
    spec.depth_share_bp = 10'001;
    EXPECT_EQ(validate(spec).error(), SpecError::BadDepthShare);
}

TEST(JoeRestingSize, LotsFitTheVenueMinimumAndTheChildCap) {
    const auto lot = d("0.001");
    const auto px = d("100");
    EXPECT_EQ(fit_lots(5, 100, lot, px, d("0"), d("1000")), 5);
    EXPECT_EQ(fit_lots(5, 100, lot, px, d("1"), d("1000")), 10);
    EXPECT_EQ(fit_lots(50, 100, lot, px, d("0"), d("2")), 20);
    EXPECT_EQ(fit_lots(5, 8, lot, px, d("1"), d("1000")), 0);
    EXPECT_EQ(fit_lots(5, 100, lot, px, d("3"), d("2")), 0);
}

TEST(JoeRestingSpec, ValidateRefusesWhatCannotRest) {
    EXPECT_TRUE(validate(iceberg_spec()).has_value());
    auto spec = iceberg_spec();
    spec.limit = FixedE9::undef();
    EXPECT_EQ(validate(spec).error(), SpecError::NoRestLimit);
    spec = iceberg_spec();
    spec.iceberg.show = FixedE9::undef();
    EXPECT_EQ(validate(spec).error(), SpecError::NoShow);
    spec = iceberg_spec("0.1", "0.2");
    EXPECT_EQ(validate(spec).error(), SpecError::ShowTooBig);
    spec = iceberg_spec();
    spec.iceberg.variance_bp = 5001;
    EXPECT_EQ(validate(spec).error(), SpecError::BadVariance);
    spec = iceberg_spec();
    spec.horizon_ns = 5;
    EXPECT_EQ(validate(spec).error(), SpecError::BadExpiry);

    EXPECT_TRUE(validate(pegged_spec()).has_value());
    spec = pegged_spec();
    spec.peg.offset_ticks = 1;
    spec.peg.offset_bp = 1;
    EXPECT_EQ(validate(spec).error(), SpecError::BothOffsets);
    spec = pegged_spec();
    spec.peg.offset_bp = 2000;
    EXPECT_EQ(validate(spec).error(), SpecError::BadOffset);
    spec = pegged_spec();
    spec.peg.threshold_ticks = 0;
    EXPECT_EQ(validate(spec).error(), SpecError::NoThreshold);
    spec = pegged_spec();
    spec.peg.min_interval_ns = 1 * k_ms;
    EXPECT_EQ(validate(spec).error(), SpecError::BadInterval);
    spec = pegged_spec(PegTo::Market);
    spec.post_only = true;
    EXPECT_EQ(validate(spec).error(), SpecError::PostOnlyCrosses);
    spec.peg.offset_ticks = 1;
    EXPECT_TRUE(validate(spec).has_value());
}

TEST(JoeIceberg, ShowsOneSizeAtTheLimitAndRefillsWhenItCompletes) {
    Fixture f;
    auto spec = iceberg_spec();
    spec.iceberg.variance_bp = 3000;
    const auto id = *f.desk.start(spec);
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent[0].tif, ChildTif::Gtc);
    EXPECT_EQ(f.sink.sent[0].limit, d("100.5"));
    EXPECT_GE(f.sink.sent[0].lots, 7);
    EXPECT_LE(f.sink.sent[0].lots, 13);

    std::int64_t total = 0;
    for (std::size_t i = 0; i < f.sink.sent.size(); ++i) {
        f.clock.advance(100 * k_ms);
        const auto lots = f.sink.sent[i].lots;
        total += lots;
        // a part fill alone does not refill in this mode
        if (lots > 1) {
            f.fill(i + 1, lots - 1);
            EXPECT_EQ(f.sink.sent.size(), i + 1);
            f.fill(i + 1, 1);
        } else {
            f.fill(i + 1, 1);
        }
        EXPECT_LE(f.live_children(id), 1U);
    }
    EXPECT_EQ(total, 100);
    EXPECT_EQ(f.parent(id).filled(), d("0.1"));
    EXPECT_EQ(f.parent(id).status(), Status::Done);
    EXPECT_GT(f.sink.sent.size(), 5U);
    // every show ended filled and none was cancelled
    EXPECT_TRUE(f.sink.cancels.empty());
}

TEST(JoeIceberg, RefillOnEachFillReplacesThePartFilledShowOneAtATime) {
    Fixture f;
    auto spec = iceberg_spec();
    spec.iceberg.refill = Refill::OnFill;
    const auto id = *f.desk.start(spec);
    ASSERT_EQ(f.sink.sent[0].lots, 10);
    f.desk.on_ack(1);
    f.clock.advance(2 * k_s);
    f.fill(1, 4);
    ASSERT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    // no second child until the venue confirms the first is gone
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].lots, 10);
    EXPECT_EQ(f.parent(id).working(), d("0.01"));
    EXPECT_EQ(f.parent(id).filled(), d("0.004"));
}

TEST(JoeIceberg, UsesTheChosenVenueOrTheBestOne) {
    Fixture f;
    f.market.touches[VenueId::Okx] = {.bid = d("99.97"), .ask = d("99.99")};
    auto spec = iceberg_spec();
    spec.venue = VenueId::Binance;
    (void)*f.desk.start(spec);
    EXPECT_EQ(f.sink.sent.at(0).instrument, on(VenueId::Binance));
    (void)*f.desk.start(iceberg_spec());
    EXPECT_EQ(f.sink.sent.at(1).instrument, on(VenueId::Okx));
}

TEST(JoeIceberg, AChosenVenueThatNeverQuotesFailsWithNothingSent) {
    Fixture f;
    auto spec = iceberg_spec();
    spec.venue = VenueId::Gate;
    const auto id = *f.desk.start(spec);
    EXPECT_TRUE(f.sink.sent.empty());
    f.pass(31 * k_s);
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
}

TEST(JoeIceberg, TheHorizonEndsItAndTheCancelMustBeAnsweredFirst) {
    Fixture f;
    auto spec = iceberg_spec();
    spec.horizon_ns = 10 * k_s;
    const auto id = *f.desk.start(spec);
    f.pass(11 * k_s);
    EXPECT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoePegged, RestsAtTheReferenceAndFollowsThroughCancelThenPlace) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent[0].limit, d("99.98"));
    EXPECT_EQ(f.sink.sent[0].lots, 100);
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    EXPECT_TRUE(f.sink.modifies.empty());
    ASSERT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    f.desk.on_closed(1, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].limit, d("100"));
    EXPECT_EQ(f.parent(id).live()->id, 2U);
    const auto& marks = f.parent(id).trace().reprices();
    ASSERT_EQ(marks.size(), 3U);
    EXPECT_EQ(marks.at(0).why, RepriceWhy::Place);
    EXPECT_EQ(marks.at(1).why, RepriceWhy::Closed);
    EXPECT_TRUE(marks.at(1).price.is_undef());
    EXPECT_EQ(marks.at(2).why, RepriceWhy::Peg);
    EXPECT_EQ(marks.at(2).price, d("100"));
}

TEST(JoePegged, ModifiesWhereTheVenueCanAndAPriceTakesOnlyOnTheAnswer) {
    Fixture f;
    f.book().amend = true;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.modifies.size(), 1U);
    EXPECT_EQ(f.sink.modifies[0].second, d("100"));
    EXPECT_TRUE(f.sink.cancels.empty());
    EXPECT_EQ(f.parent(id).children()[0].limit, d("99.98"));
    // one at a time, whatever the book does meanwhile
    f.quote("100.50", "100.52");
    f.pass(2 * k_s);
    EXPECT_EQ(f.sink.modifies.size(), 1U);

    f.desk.on_modified(1);
    EXPECT_EQ(f.parent(id).children()[0].limit, d("100"));
    EXPECT_EQ(f.parent(id).trace().reprices().last()->price, d("100"));
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.modifies.size(), 2U);
}

TEST(JoePegged, ARefusedOrUnansweredModifyFallsBackToCancelThenPlace) {
    Fixture f;
    f.book().amend = true;
    (void)*f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.modifies.size(), 1U);
    f.desk.on_modify_refused(1);
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.modifies.size(), 1U);
    EXPECT_EQ(f.sink.cancels.size(), 1U);

    Fixture silent;
    silent.book().amend = true;
    (void)*silent.desk.start(pegged_spec());
    silent.desk.on_ack(1);
    silent.quote("100.00", "100.02");
    silent.pass(1 * k_s);
    ASSERT_EQ(silent.sink.modifies.size(), 1U);
    silent.pass(4 * k_s);
    EXPECT_TRUE(silent.sink.cancels.empty());
    silent.pass(2 * k_s);
    EXPECT_EQ(silent.sink.cancels.size(), 1U);

    Fixture refused;
    refused.book().amend = true;
    refused.sink.modify_goes = false;
    (void)*refused.desk.start(pegged_spec());
    refused.desk.on_ack(1);
    refused.quote("100.00", "100.02");
    refused.pass(1 * k_s);
    EXPECT_EQ(refused.sink.cancels.size(), 1U);
}

TEST(JoePegged, ThresholdAndIntervalHoldBackTheRepeg) {
    Fixture f;
    auto spec = pegged_spec();
    spec.peg.threshold_ticks = 3;
    spec.peg.min_interval_ns = 2 * k_s;
    (void)*f.desk.start(spec);
    f.desk.on_ack(1);

    // past the threshold but inside the interval
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    EXPECT_TRUE(f.sink.cancels.empty());
    // the interval has passed but the price came back inside the threshold
    f.quote("99.99", "100.02");
    f.pass(2 * k_s);
    EXPECT_TRUE(f.sink.cancels.empty());
    // both
    f.quote("100.01", "100.03");
    for (int i = 0; i < 20; ++i) {
        f.pass(250 * k_ms);
    }
    EXPECT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoePegged, ABookThatFlickersEveryTickStaysWithinTheInterval) {
    Fixture f;
    auto spec = pegged_spec();
    spec.peg.min_interval_ns = 5 * k_s;
    (void)*f.desk.start(spec);
    f.desk.on_ack(1);
    std::size_t acted = 0;
    for (int i = 0; i < 240; ++i) {
        f.quote(i % 2 == 0 ? "100.10" : "100.20", "100.30");
        f.pass(250 * k_ms);
        if (const auto kid = f.sink.sent.size(); f.sink.cancels.size() > acted) {
            acted = f.sink.cancels.size();
            f.desk.on_closed(f.sink.cancels.back(), ChildEnd::Cancelled);
            f.desk.on_ack(f.sink.sent.size());
            EXPECT_EQ(f.sink.sent.size(), kid + 1);
        }
    }
    // a minute at one change per five seconds
    EXPECT_LE(f.sink.cancels.size(), 12U);
    EXPECT_GE(f.sink.cancels.size(), 10U);
}

TEST(JoePegged, AnUnconfirmedCancelBlocksTheReplacementAndIsChased) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.cancels.size(), 1U);
    for (int i = 0; i < 8; ++i) {
        f.quote(i % 2 == 0 ? "100.10" : "100.20", "100.30");
        f.pass(1 * k_s);
        EXPECT_EQ(f.sink.sent.size(), 1U);
        EXPECT_EQ(f.live_children(id), 1U);
    }
    EXPECT_EQ(f.sink.cancels.size(), 2U);
    // a refused cancel means the child still rests, so the next decision is free to send another
    f.desk.on_cancel_refused(1);
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.cancels.size(), 3U);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    EXPECT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.live_children(id), 1U);
}

TEST(JoePegged, AFullFillWhileCancellingBooksAndNeedsNoReplacement) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec(PegTo::Primary, "0.1"));
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.cancels.size(), 1U);
    f.fill(1, 100);
    // the venue answers the cancel it could not do
    f.desk.on_cancel_refused(1);
    f.pass(1 * k_s);
    EXPECT_EQ(f.parent(id).filled(), d("0.1"));
    EXPECT_EQ(f.parent(id).status(), Status::Done);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoePegged, APartFillWhileCancellingSizesTheReplacementToWhatIsLeft) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    f.fill(1, 30);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].lots, 70);
    EXPECT_EQ(f.parent(id).filled(), d("0.03"));
    EXPECT_EQ(f.parent(id).working(), d("0.07"));
}

TEST(JoePegged, ALateFillAfterTheReplacementShrinksIt) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.at(1).lots, 100);
    f.desk.on_ack(2);
    // the first child traded before it was cancelled and the fill crossed the cancel on the wire
    f.fill(1, 40);
    EXPECT_EQ(f.parent(id).filled(), d("0.04"));
    EXPECT_EQ(f.sink.cancels.back(), 2U);
    EXPECT_EQ(f.sink.sent.size(), 2U);
    f.desk.on_closed(2, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 3U);
    EXPECT_EQ(f.sink.sent[2].lots, 60);
}

TEST(JoePegged, APartFillDuringAModifyEndsInTheChildBeingReplaced) {
    Fixture f;
    f.book().amend = true;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.modifies.size(), 1U);
    f.fill(1, 25, "99.98");
    EXPECT_EQ(f.parent(id).filled(), d("0.025"));
    f.desk.on_modified(1);
    // an amend that crossed a fill may have restated the size on some venues
    ASSERT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    f.desk.on_closed(1, ChildEnd::Cancelled);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].lots, 75);
    EXPECT_EQ(f.sink.sent[1].limit, d("100"));
    EXPECT_EQ(f.live_children(id), 1U);
}

TEST(JoePegged, AChildWithAFillNeverGoesThroughAModify) {
    Fixture f;
    f.book().amend = true;
    (void)*f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.fill(1, 10);
    f.quote("100.00", "100.02");
    f.pass(1 * k_s);
    EXPECT_TRUE(f.sink.modifies.empty());
    EXPECT_EQ(f.sink.cancels.size(), 1U);
}

TEST(JoePegged, APostOnlyRejectWaitsOutTheIntervalAndRepricesFromAFreshBook) {
    Fixture f;
    auto spec = pegged_spec();
    spec.post_only = true;
    const auto id = *f.desk.start(spec);
    ASSERT_EQ(f.sink.sent.at(0).tif, ChildTif::PostOnly);
    f.desk.on_closed(1, ChildEnd::Rejected);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    EXPECT_EQ(f.parent(id).trace().reprices().last()->why, RepriceWhy::Rejected);
    f.pass(500 * k_ms);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.quote("99.90", "99.92");
    f.pass(500 * k_ms);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].limit, d("99.90"));
}

TEST(JoePegged, AVenueThatCancelsOrExpiresAPostOnlyIsTheSameRejection) {
    Fixture f;
    auto spec = pegged_spec();
    spec.post_only = true;
    const auto id = *f.desk.start(spec);
    f.desk.on_closed(1, ChildEnd::Expired);
    EXPECT_EQ(f.parent(id).children()[0].end, ChildEnd::Rejected);
    f.pass(1 * k_s);
    f.desk.on_closed(2, ChildEnd::Cancelled);
    EXPECT_EQ(f.parent(id).children()[1].end, ChildEnd::Rejected);
    Tca tca;
    tca.add(f.parent(id));
    EXPECT_EQ(tca.venues()[0].rejected, 2U);
}

TEST(JoePegged, RejectionsInARowFailTheParentWithNothingLiveAndAFillClearsTheCount) {
    Fixture f;
    auto spec = pegged_spec();
    spec.post_only = true;
    const auto id = *f.desk.start(spec);
    for (std::uint64_t kid = 1; kid <= 4; ++kid) {
        f.desk.on_closed(kid, ChildEnd::Rejected);
        f.pass(1 * k_s);
    }
    f.desk.on_ack(5);
    f.fill(5, 1);
    EXPECT_EQ(f.parent(id).slot().rejects, 0U);
    f.desk.on_closed(5, ChildEnd::Expired);
    f.pass(1 * k_s);
    for (std::uint64_t kid = 6; kid <= 10; ++kid) {
        f.desk.on_closed(kid, ChildEnd::Rejected);
        f.pass(1 * k_s);
    }
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
    EXPECT_NE(f.parent(id).reason().find("turned away 5"), std::string_view::npos);
    EXPECT_EQ(f.live_children(id), 0U);
    EXPECT_EQ(f.sink.sent.size(), 10U);
}

TEST(JoePegged, AVenueWithoutPostOnlyFailsRatherThanSendingATaker) {
    Fixture f;
    f.book().post_only = false;
    auto spec = pegged_spec();
    spec.post_only = true;
    spec.venue = VenueId::Binance;
    const auto id = *f.desk.start(spec);
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
    EXPECT_TRUE(f.sink.sent.empty());
}

TEST(JoePegged, CancellingTheParentLeavesNothingLive) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    ASSERT_TRUE(f.desk.cancel(id).has_value());
    EXPECT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    EXPECT_EQ(f.parent(id).status(), Status::Cancelled);

    // the answer is slow: joe keeps asking though the parent is over, and sends nothing new
    f.quote("100.00", "100.02");
    f.pass(5 * k_s);
    EXPECT_EQ(f.sink.cancels.size(), 2U);
    EXPECT_EQ(f.sink.sent.size(), 1U);

    f.fill(1, 10);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    EXPECT_EQ(f.live_children(id), 0U);
    EXPECT_EQ(f.parent(id).filled(), d("0.01"));
    f.pass(30 * k_s);
    EXPECT_EQ(f.sink.cancels.size(), 2U);
    EXPECT_EQ(f.sink.sent.size(), 1U);
}

TEST(JoePegged, AFinishedParentWithAChildOutIsNotPruned) {
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Desk desk{market, sink, clock, curves, {.keep = 1}};
    const auto first = *desk.start(pegged_spec());
    ASSERT_TRUE(desk.cancel(first).has_value());
    (void)*desk.start(pegged_spec());
    (void)*desk.start(pegged_spec());
    EXPECT_NE(desk.find(first), nullptr);
    desk.on_closed(1, ChildEnd::Cancelled);
    (void)*desk.start(pegged_spec());
    EXPECT_EQ(desk.find(first), nullptr);
}

TEST(JoePegged, PauseAndResumeReplaceOnlyAfterTheCancelIsAnswered) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    ASSERT_TRUE(f.desk.pause(id).has_value());
    EXPECT_EQ(f.sink.cancels.size(), 1U);
    ASSERT_TRUE(f.desk.resume(id).has_value());
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    EXPECT_EQ(f.sink.sent.size(), 2U);
}

TEST(JoePegged, AStaleBookPullsTheChildAndPausesUntilItIsBack) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.book().fresh = false;
    f.pass(250 * k_ms);
    EXPECT_EQ(f.sink.cancels, std::vector<std::uint64_t>{1});
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Stale);
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    f.pass(10 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Stale);
    // recorded once, however long it lasts
    const auto& marks = f.parent(id).trace().reprices();
    std::size_t stale = 0;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (marks.at(i).why == RepriceWhy::Stale) {
            ++stale;
        }
    }
    EXPECT_EQ(stale, 1U);

    f.book().fresh = true;
    f.quote("100.00", "100.02");
    f.pass(250 * k_ms);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.sink.sent[1].limit, d("100"));
    EXPECT_EQ(f.parent(id).slot().hold, Hold::None);
}

TEST(JoePegged, ACrossedBookPausesToo) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.quote("100.05", "100.05");
    f.pass(250 * k_ms);
    EXPECT_EQ(f.sink.cancels.size(), 1U);
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Crossed);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    f.pass(5 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    const auto& marks = f.parent(id).trace().reprices();
    EXPECT_EQ(marks.at(1).why, RepriceWhy::Crossed);
    EXPECT_TRUE(marks.last()->price.is_undef());
}

TEST(JoeIceberg, AStaleBookStopsTheNextShowButLeavesTheLimitOrderResting) {
    Fixture f;
    const auto id = *f.desk.start(iceberg_spec());
    f.desk.on_ack(1);
    f.book().fresh = false;
    f.pass(1 * k_s);
    EXPECT_TRUE(f.sink.cancels.empty());
    f.fill(1, 10);
    EXPECT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.parent(id).slot().hold, Hold::Stale);
    f.book().fresh = true;
    f.pass(1 * k_s);
    EXPECT_EQ(f.sink.sent.size(), 2U);
}

TEST(JoePegged, AReconClosedChildIsOwedAgainAsForAnIoc) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    f.desk.on_ack(1);
    f.desk.on_closed(1, ChildEnd::Lost);
    EXPECT_EQ(f.parent(id).children()[0].end, ChildEnd::Lost);
    EXPECT_EQ(f.parent(id).working(), d("0"));
    EXPECT_EQ(f.parent(id).remaining(), d("0.1"));
    EXPECT_EQ(f.sink.sent.size(), 1U);
    f.pass(1 * k_s);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    f.desk.on_ack(2);
    f.fill(2, 20);
    f.desk.on_closed(2, ChildEnd::Lost);
    ASSERT_EQ(f.sink.sent.size(), 3U);
    EXPECT_EQ(f.sink.sent[2].lots, 80);
    EXPECT_EQ(f.live_children(id), 1U);
}

TEST(JoePegged, TheChildCapSizesEachChildAndTheNextFollowsTheFill) {
    Fixture f;
    auto spec = pegged_spec();
    spec.max_child_notional = d("2");
    const auto id = *f.desk.start(spec);
    ASSERT_EQ(f.sink.sent.at(0).lots, 20);
    f.fill(1, 20);
    ASSERT_EQ(f.sink.sent.size(), 2U);
    EXPECT_EQ(f.parent(id).working(), d("0.02"));
}

TEST(JoePegged, ARiskRefusalRepeatsAtTheResendPaceUntilItFails) {
    Fixture f;
    f.sink.refuse = SinkError::Risk;
    const auto id = *f.desk.start(pegged_spec());
    EXPECT_EQ(f.parent(id).status(), Status::Working);
    f.pass(1 * k_s);
    f.pass(1 * k_s);
    EXPECT_EQ(f.parent(id).status(), Status::Failed);
}

TEST(JoeLateFill, AFillPastWhatWasLeftBooksOnlyWhatWasLeft) {
    Fixture f;
    ParentSpec spec{};
    spec.listing = btc();
    spec.qty = d("0.1");
    spec.algo = Algo::Sor;
    spec.max_child_notional = d("1000");
    const auto id = *f.desk.start(spec);
    ASSERT_EQ(f.sink.sent.at(0).lots, 100);
    f.fill(1, 30);
    f.desk.on_closed(1, ChildEnd::Cancelled);
    // strategyd's closed-order book holds 70 lots here and takes no more into the position
    f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(95), PriceTicks::from_raw(10000), d("0.002"));
    const auto& parent = f.parent(id);
    EXPECT_EQ(parent.filled(), d("0.1"));
    EXPECT_EQ(parent.children()[0].filled, d("0.1"));
    EXPECT_EQ(parent.value(), value_of(d("0.03"), d("99.98")) + value_of(d("0.07"), d("100")));
    EXPECT_EQ(parent.fees(), d("0.003"));
    EXPECT_EQ(parent.trace().fills().last()->qty, d("0.07"));

    // nothing is left, so a resend of it books nothing at all, fee included
    f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(5), PriceTicks::from_raw(10000), d("0.002"));
    EXPECT_EQ(parent.filled(), d("0.1"));
    EXPECT_EQ(parent.fees(), d("0.003"));
}

TEST(JoeLateFill, AnOpenChildFollowsTheLeavesTheWayThePositionDoes) {
    Fixture f;
    const auto id = *f.desk.start(pegged_spec());
    // a fill reported at 10 lots whose leaves dropped by 30: strategyd books the 30
    f.desk.on_fill(
        1, on(VenueId::Binance), QtyLots::from_raw(10), PriceTicks::from_raw(9998), d("0"), QtyLots::from_raw(70));
    EXPECT_EQ(f.parent(id).filled(), d("0.03"));
    // leaves that did not drop book nothing
    f.desk.on_fill(
        1, on(VenueId::Binance), QtyLots::from_raw(10), PriceTicks::from_raw(9998), d("0"), QtyLots::from_raw(70));
    EXPECT_EQ(f.parent(id).filled(), d("0.03"));
    // undef leaves, a pulled fill: its own size, capped
    f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(500), PriceTicks::from_raw(9998), d("0"));
    EXPECT_EQ(f.parent(id).filled(), d("0.1"));
    EXPECT_FALSE(f.parent(id).children()[0].open);
    EXPECT_EQ(f.parent(id).children()[0].end, ChildEnd::Filled);
}

TEST(JoeChildren, ALongRunHoldsABoundedListAndItsTotalsStayExact) {
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    DeskConfig config{};
    config.keep_children = 16;
    Desk desk{market, sink, clock, curves, config};
    auto spec = pegged_spec(PegTo::Primary, "1000");
    const auto id = *desk.start(spec);

    Tca truth;
    std::int64_t lots = 0;
    for (std::uint64_t kid = 1; kid <= 3000; ++kid) {
        clock.advance(1 * k_ms);
        desk.on_ack(kid);
        clock.advance(1 * k_s);
        if (kid % 3 == 0) {
            desk.on_fill(kid, on(VenueId::Binance), QtyLots::from_raw(2), PriceTicks::from_raw(9998), d("0.001"));
            lots += 2;
        }
        const auto end = kid % 7 == 0 ? ChildEnd::Rejected : ChildEnd::Cancelled;
        (void)desk.pause(id);
        desk.on_closed(kid, end);
        (void)desk.resume(id);
        desk.tick();
        ASSERT_EQ(sink.sent.size(), kid + 1);
    }
    const auto& parent = *desk.find(id);
    EXPECT_LE(parent.children().size(), 16U);
    EXPECT_EQ(parent.child_count(), 3001U);
    EXPECT_EQ(parent.retired_count() + parent.children().size(), 3001U);
    EXPECT_EQ(parent.filled(), d("0.001").scaled_by(lots));
    EXPECT_EQ(parent.fees(), d("0.001").scaled_by(1000));

    Tca tca;
    tca.add(parent);
    const auto venues = tca.venues();
    ASSERT_EQ(venues.size(), 1U);
    EXPECT_EQ(venues[0].children, 3001U);
    EXPECT_EQ(venues[0].filled_children, 1000U);
    EXPECT_EQ(venues[0].filled, parent.filled());
    EXPECT_EQ(venues[0].value, parent.value());
    EXPECT_EQ(venues[0].fees, parent.fees());
    EXPECT_EQ(venues[0].rejected, 3000U / 7);
    EXPECT_EQ(venues[0].cancelled, 3000U - 3000U / 7);
    EXPECT_EQ(venues[0].acked, 3000U);
    EXPECT_EQ(venues[0].ack_max_ns, 1 * k_ms);
    EXPECT_EQ(venues[0].ack_p50_ns, 1 * k_ms);
}

TEST(JoeChildren, ALateFillOnARetiredChildStillBooksAndAgesOut) {
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    DeskConfig config{};
    config.keep_children = 2;
    Desk desk{market, sink, clock, curves, config};
    const auto id = *desk.start(pegged_spec(PegTo::Primary, "1"));
    for (std::uint64_t kid = 1; kid <= 6; ++kid) {
        (void)desk.pause(id);
        desk.on_closed(kid, ChildEnd::Cancelled);
        (void)desk.resume(id);
        clock.advance(1 * k_s);
        desk.tick();
    }
    const auto& parent = *desk.find(id);
    ASSERT_EQ(parent.child(1), nullptr);
    EXPECT_TRUE(parent.owns(1));

    // capped by what the child had left, on the totals and the venue's row alike
    desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(9000), PriceTicks::from_raw(9998), d("0.001"));
    EXPECT_EQ(parent.filled(), d("1"));
    Tca tca;
    tca.add(parent);
    EXPECT_EQ(tca.venues()[0].filled, d("1"));
    EXPECT_EQ(tca.venues()[0].filled_children, 1U);
    EXPECT_EQ(tca.venues()[0].value, parent.value());

    // strategyd stops attributing a fill after its window, and so does joe
    clock.advance(16 * 60 * k_s);
    desk.tick();
    EXPECT_FALSE(parent.owns(2));
    desk.on_fill(2, on(VenueId::Binance), QtyLots::from_raw(5), PriceTicks::from_raw(9998), d("0.001"));
    EXPECT_EQ(parent.fees(), d("0.001"));
}

TEST(JoeRoutes, TakesAnIcebergAndAPegAndAnswersTheirFields) {
    Fixture f;
    const JoeRoutes routes{&f.desk};
    const auto iceberg = routes(
        {.method = "POST",
         .path = "/joe/parents",
         .body = R"({"symbol":"BTC/USDT","side":"buy","qty":"0.1","limit":"100.5","algo":"iceberg",
                                     "venue":"binance","post_only":true,
                                     "iceberg":{"show":"0.01","variance_bp":1000,"refill":"fill"}})"});
    ASSERT_EQ(iceberg->status, 201) << iceberg->body;
    const auto pegged = routes({.method = "POST",
                                .path = "/joe/parents",
                                .body = R"({"symbol":"BTC/USDT","side":"sell","qty":"0.1","algo":"pegged",
                                    "pegged":{"to":"mid","offset_ticks":-1,"threshold_ticks":2,"min_interval_ms":500}})"});
    ASSERT_EQ(pegged->status, 201) << pegged->body;

    const auto one = routes({.method = "GET", .path = "/joe/parents/1"});
    EXPECT_NE(one->body.find(R"("iceberg":{"show":"0.01","variance_bp":1000,"refill":"fill"})"), std::string::npos);
    EXPECT_NE(one->body.find(R"("venue":"binance","post_only":true)"), std::string::npos);
    EXPECT_NE(one->body.find(R"("tif":"post_only")"), std::string::npos);
    EXPECT_NE(one->body.find(R"("working_price":"99.99")"), std::string::npos);
    const auto two = routes({.method = "GET", .path = "/joe/parents/2"});
    EXPECT_NE(two->body.find(
                  R"("pegged":{"to":"mid","offset_ticks":-1,"offset_bp":0,"threshold_ticks":2,"min_interval_ms":500})"),
              std::string::npos);

    const auto trace = routes({.method = "GET", .path = "/joe/parents/2/trace"});
    EXPECT_NE(trace->body.find(R"("reprices":[{"seq":)"), std::string::npos);
    EXPECT_NE(trace->body.find(R"("why":"place")"), std::string::npos);
    EXPECT_NE(trace->body.find(R"("price":"99.98")"), std::string::npos);
    const auto later = routes({.method = "GET", .path = "/joe/parents/2/trace", .query = "since=99"});
    EXPECT_NE(later->body.find(R"("reprices":[])"), std::string::npos);
}

TEST(JoeRoutes, RefusesARestingBodyThatDoesNotAddUp) {
    Fixture f;
    const JoeRoutes routes{&f.desk};
    const auto post = [&](std::string body) {
        return routes({.method = "POST", .path = "/joe/parents", .body = std::move(body)})->status;
    };
    const std::string head = R"({"symbol":"BTC/USDT","side":"buy","qty":"0.1",)";
    EXPECT_EQ(post(head + R"("algo":"iceberg","limit":"100"})"), 400);
    EXPECT_EQ(post(head + R"("algo":"iceberg","iceberg":{"show":"0.01"}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"iceberg","limit":"100","iceberg":{"show":"x"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"iceberg","limit":"100","iceberg":{"show":"0.01","refill":"x"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"sor","pegged":{}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"sor","post_only":true})"), 400);
    EXPECT_EQ(post(head + R"("algo":"pegged","venue":"nowhere"})"), 400);
    EXPECT_EQ(post(head + R"("algo":"pegged","pegged":{"to":"x"}})"), 400);
    EXPECT_EQ(post(head + R"("algo":"pegged","pegged":{"min_interval_ms":1}})"), 422);
    EXPECT_EQ(post(head + R"("algo":"pegged"})"), 201);
}

} // namespace
} // namespace raikiri::joe
