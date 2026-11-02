#include <raikiri/joe/desk.hpp>
#include <raikiri/joe/routes.hpp>
#include <raikiri/joe/tca.hpp>
#include <raikiri/joe/trace.hpp>

#include <gtest/gtest.h>

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

class Market final : public MarketView {
public:
    std::vector<VenueBook> books(const Listing&, Side side) const override {
        if (side == Side::Buy) {
            return {book(VenueId::Binance, ask), book(VenueId::Okx, ask + d("0.02"))};
        }
        return {book(VenueId::Binance, bid), book(VenueId::Okx, bid - d("0.02"))};
    }

    std::optional<Scale> scale(InstrumentId) const override {
        return Scale{.listing = btc(), .tick = d("0.01"), .lot = d("0.001")};
    }

    FixedE9 ask{d("100")};
    FixedE9 bid{d("99.98")};

private:
    static VenueBook book(VenueId id, FixedE9 px) {
        return {.instrument = on(id), .lot = d("0.001"), .levels = {{px, d("1")}}};
    }
};

class Sink final : public OrderSink {
public:
    bool running() const noexcept override { return true; }

    std::expected<std::uint64_t, SinkError> submit(const ChildOrder& order) noexcept override {
        sent.push_back(order);
        return sent.size();
    }

    void cancel(std::uint64_t) noexcept override {}

    std::vector<ChildOrder> sent;
};

struct Fixture {
    Market market;
    Sink sink;
    TestClock clock{UnixNanos{1000 * k_s}};
    CurveProfile curves;
    Desk desk{market, sink, clock, curves};

    std::uint64_t twap() {
        ParentSpec spec{};
        spec.listing = btc();
        spec.qty = d("0.1");
        spec.horizon_ns = 60 * k_s;
        spec.slices = 4;
        return *desk.start(spec);
    }
};

TEST(JoeTrace, RingKeepsTheNewestAndCountsWhatItDropped) {
    Ring<int> ring;
    ring.open(3);
    for (int i = 1; i <= 5; ++i) {
        ring.push(i);
    }
    EXPECT_EQ(ring.size(), 3U);
    EXPECT_EQ(ring.capacity(), 3U);
    EXPECT_EQ(ring.dropped(), 2U);
    EXPECT_EQ(ring.at(0), 3);
    EXPECT_EQ(*ring.last(), 5);
}

TEST(JoeTrace, ShapeCoversTheRunWithinTheCap) {
    const auto quick = trace_shape(30 * k_s, k_s, 1024);
    EXPECT_EQ(quick.interval_ns, k_s);
    EXPECT_EQ(quick.mids, 32U);
    const auto week = trace_shape(7 * 86400 * k_s, k_s, 1024);
    EXPECT_EQ(week.mids, 1024U);
    EXPECT_GE(week.interval_ns * 1023, 7 * 86400 * k_s);
}

TEST(JoeTrace, SamplesWaitOutTheIntervalAndSeqOrdersEverything) {
    Trace trace;
    trace.open(k_s, 4, 2);
    EXPECT_TRUE(trace.sample(UnixNanos{k_s}, d("100"), FixedE9::undef()));
    EXPECT_FALSE(trace.sample(UnixNanos{k_s + 10}, d("101"), FixedE9::undef()));
    trace.fill({.ts = UnixNanos{k_s + 20}, .child_id = 9, .qty = d("1"), .price = d("100")});
    EXPECT_TRUE(trace.sample(UnixNanos{2 * k_s}, d("102"), d("101")));
    EXPECT_EQ(trace.mids().size(), 2U);
    EXPECT_EQ(trace.fills().at(0).seq, 2U);
    EXPECT_EQ(trace.mids().at(1).seq, 3U);
    EXPECT_EQ(trace.seq(), 3U);
}

TEST(JoeDesk, TraceRecordsArrivalMidsFillsAndAcks) {
    Fixture f;
    const auto id = f.twap();
    ASSERT_EQ(f.sink.sent.size(), 1U);
    EXPECT_EQ(f.sink.sent[0].instrument, on(VenueId::Binance));

    f.clock.advance(4 * k_ms);
    f.desk.on_ack(1);
    f.clock.advance(1 * k_ms);
    f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(25), PriceTicks::from_raw(10001), d("0.001"));
    f.desk.on_closed(1, ChildEnd::Filled);
    // a late expiry does not overwrite how the child ended
    f.desk.on_closed(1, ChildEnd::Expired);

    f.market.ask = d("100.10");
    f.market.bid = d("100.06");
    f.clock.advance(1 * k_s);
    f.desk.tick();

    const auto* parent = f.desk.find(id);
    ASSERT_NE(parent, nullptr);
    const auto& child = parent->children().front();
    EXPECT_EQ(child.acked.ns - child.sent.ns, 4 * k_ms);
    EXPECT_EQ(child.end, ChildEnd::Filled);

    const auto& trace = parent->trace();
    ASSERT_EQ(trace.mids().size(), 2U);
    EXPECT_EQ(trace.mids().at(0).mid, d("99.99"));
    EXPECT_TRUE(trace.mids().at(0).vwap.is_undef());
    EXPECT_EQ(trace.mids().at(1).mid, d("100.08"));
    ASSERT_EQ(trace.fills().size(), 1U);
    EXPECT_EQ(trace.fills().at(0).ts.ns, 1000 * k_s + 5 * k_ms);
    EXPECT_EQ(trace.fills().at(0).price, d("100.01"));
    EXPECT_EQ(trace.fills().at(0).qty, d("0.025"));

    Tca tca;
    tca.add(*parent);
    const auto venues = tca.venues();
    ASSERT_EQ(venues.size(), 1U);
    EXPECT_EQ(venues[0].venue, VenueId::Binance);
    EXPECT_EQ(venues[0].filled, d("0.025"));
    EXPECT_EQ(venues[0].ack_p50_ns, 4 * k_ms);
    EXPECT_EQ(bp_of(venues[0].cost, venues[0].benchmark), d("2"));
    EXPECT_EQ(bp_of(venues[0].fees, venues[0].value), d("3.999"));
}

TEST(JoeDesk, TraceBuffersAreSizedOnceAtStart) {
    Fixture f;
    const auto id = f.twap();
    const auto* parent = f.desk.find(id);
    const auto mids = parent->trace().mids().capacity();
    const auto fills = parent->trace().fills().capacity();
    for (int i = 0; i < 400; ++i) {
        f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(0), PriceTicks::from_raw(10000), d("0"));
        f.clock.advance(1 * k_s);
        f.desk.tick();
    }
    EXPECT_EQ(parent->trace().mids().capacity(), mids);
    EXPECT_EQ(parent->trace().fills().capacity(), fills);
    EXPECT_GT(parent->trace().fills().dropped(), 0U);
}

TEST(JoeRoutes, TraceAnswersOnlyWhatIsNewerThanSince) {
    Fixture f;
    const auto id = f.twap();
    f.desk.on_fill(1, on(VenueId::Binance), QtyLots::from_raw(25), PriceTicks::from_raw(10001), d("0"));
    const JoeRoutes routes{&f.desk};
    const auto path = "/joe/parents/" + std::to_string(id) + "/trace";

    const auto full = routes({.method = "GET", .path = path});
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->status, 200);
    EXPECT_NE(full->body.find(R"("mid":"99.99")"), std::string::npos);
    EXPECT_NE(full->body.find(R"("price":"100.01")"), std::string::npos);
    EXPECT_NE(full->body.find(R"("plan":[)"), std::string::npos);
    EXPECT_NE(full->body.find(R"("venue":"binance")"), std::string::npos);

    const auto none = routes({.method = "GET", .path = path, .query = "since=2"});
    ASSERT_TRUE(none.has_value());
    EXPECT_NE(none->body.find(R"("mids":[],"fills":[])"), std::string::npos);
    EXPECT_EQ(none->body.find(R"("plan")"), std::string::npos);

    const auto bad = routes({.method = "GET", .path = path, .query = "since=x"});
    EXPECT_EQ(bad->status, 400);

    const auto tca = routes({.method = "GET", .path = "/joe/tca"});
    ASSERT_TRUE(tca.has_value());
    EXPECT_NE(tca->body.find(R"("parents":1)"), std::string::npos);
}

TEST(JoeTag, ASecondStartWithATagStillHeldIsTheFirstParent) {
    Market market;
    Sink sink;
    CurveProfile curves;
    TestClock clock{UnixNanos{1'000 * k_s}};
    Desk desk{market, sink, clock, curves};
    JoeRoutes routes{&desk};
    constexpr std::string_view k_body = R"({"symbol":"BTC/USDT","side":"buy","qty":"0.1","algo":"sor",)"
                                        R"("max_child_notional":"100","client_tag":"parent-1-0"})";
    const auto first = routes(http::Request{.method = "POST", .path = "/joe/parents", .body = k_body});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->status, 201) << first->body;
    const auto again = routes(http::Request{.method = "POST", .path = "/joe/parents", .body = k_body});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->status, 200) << again->body;
    EXPECT_EQ(again->body, first->body);
    EXPECT_EQ(desk.parents().size(), 1U);

    const auto found = routes(http::Request{.method = "GET", .path = "/joe/parents", .query = "client_tag=parent-1-0"});
    EXPECT_NE(found->body.find(R"("client_tag":"parent-1-0")"), std::string::npos) << found->body;
    const auto none = routes(http::Request{.method = "GET", .path = "/joe/parents", .query = "client_tag=other"});
    EXPECT_EQ(none->body, R"({"items":[]})");

    ParentSpec untagged{.listing = btc(), .qty = d("0.1"), .algo = Algo::Sor, .max_child_notional = d("100")};
    EXPECT_NE(desk.start(untagged).value(), desk.start(untagged).value());
    untagged.client_tag = std::string(65, 'x');
    EXPECT_FALSE(desk.start(untagged).has_value());
}

} // namespace
} // namespace raikiri::joe
