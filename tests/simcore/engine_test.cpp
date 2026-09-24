#include <raikiri/common/ulid.hpp>
#include <raikiri/simcore/engine.hpp>
#include <raikiri/simcore/mdgen.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace raikiri::simcore {
namespace {

constexpr InstrumentId k_inst = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{7});
constexpr UnixNanos k_t0{1'700'000'000'000'000'000ULL};

[[nodiscard]] constexpr PriceTicks px(std::int64_t v) noexcept {
    return PriceTicks::from_raw(v);
}

[[nodiscard]] constexpr QtyLots lots(std::int64_t v) noexcept {
    return QtyLots::from_raw(v);
}

// deterministic per-seq ulid
[[nodiscard]] ExchangeClientOrderId test_ulid(std::uint64_t seq) {
    UlidGen gen{seq + 1};
    return gen.next(k_t0);
}

[[nodiscard]] OrderCmd make_limit(std::uint64_t seq,
                                  Side side,
                                  PriceTicks price,
                                  QtyLots qty,
                                  TimeInForce tif = TimeInForce::Gtc,
                                  OrderFlags flags = OrderFlags::None) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.tif = tif;
    cmd.flags = flags;
    cmd.instrument = k_inst;
    cmd.client_order_id = ClientOrderId::make(1, seq);
    cmd.exchange_client_order_id = test_ulid(seq);
    cmd.price = price;
    cmd.qty = qty;
    cmd.ts_created = k_t0;
    return cmd;
}

[[nodiscard]] OrderCmd make_market(std::uint64_t seq, Side side, QtyLots qty) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = side;
    cmd.order_type = OrderType::Market;
    cmd.instrument = k_inst;
    cmd.client_order_id = ClientOrderId::make(1, seq);
    cmd.exchange_client_order_id = test_ulid(seq);
    cmd.qty = qty; // price stays undef: market orders have no limit price
    cmd.ts_created = k_t0;
    return cmd;
}

[[nodiscard]] OrderCmd
make_cancel(std::uint64_t seq, ExchangeOrderId target, ExchangeClientOrderId exch_client = {}) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Cancel;
    cmd.instrument = k_inst;
    cmd.client_order_id = ClientOrderId::make(1, seq);
    cmd.exchange_client_order_id = exch_client;
    cmd.target_exchange_order_id = target;
    cmd.ts_created = k_t0;
    return cmd;
}

[[nodiscard]] OrderCmd
make_amend(std::uint64_t seq, ExchangeOrderId target, PriceTicks price, QtyLots qty) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Amend;
    cmd.instrument = k_inst;
    cmd.client_order_id = ClientOrderId::make(1, seq);
    cmd.target_exchange_order_id = target;
    cmd.price = price;
    cmd.qty = qty;
    cmd.ts_created = k_t0;
    return cmd;
}

template <typename T> void append_pod(std::string& buf, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    buf.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void serialize(const SimResult& r, std::string& buf) {
    for (const OrderEvent& ev : r.events) {
        append_pod(buf, ev);
    }
    for (const TradeTick& trade : r.trades) {
        append_pod(buf, trade);
    }
    buf.push_back(r.bbo_changed ? '\x01' : '\x00');
}

TEST(Engine, ClientLimitCrossesBackgroundLiquidity) {
    Engine eng;
    SimResult scenery;
    const std::uint64_t ask1 =
        eng.place_background_limit(k_inst, Side::Sell, px(101), lots(5), k_t0, scenery);
    const std::uint64_t ask2 =
        eng.place_background_limit(k_inst, Side::Sell, px(102), lots(5), k_t0, scenery);
    EXPECT_NE(ask1, ask2);
    EXPECT_TRUE(scenery.events.empty()); // background liquidity is scenery
    EXPECT_TRUE(scenery.trades.empty());

    const auto r = eng.handle(make_limit(1, Side::Buy, px(102), lots(8)), k_t0 + 1);
    ASSERT_EQ(r.events.size(), 2U);
    const OrderEvent& e0 = r.events[0];
    EXPECT_EQ(e0.type, OrderEventType::Filled);
    EXPECT_EQ(e0.liquidity, Liquidity::Taker);
    EXPECT_EQ(e0.side, Side::Buy);
    EXPECT_EQ(e0.instrument, k_inst);
    EXPECT_EQ(e0.client_order_id, ClientOrderId::make(1, 1));
    EXPECT_EQ(e0.exchange_client_order_id, test_ulid(1));
    EXPECT_EQ(e0.exchange_order_id.view(), "3"); // sim ids 1,2 went to background
    EXPECT_EQ(e0.exec_id.view(), "S-1");
    EXPECT_EQ(e0.price, px(101)); // taker pays RESTING prices
    EXPECT_EQ(e0.qty, lots(5));
    EXPECT_EQ(e0.leaves_qty, lots(3));
    EXPECT_EQ(e0.fee, FixedE9::from_raw(0));
    EXPECT_EQ(e0.ts_event, k_t0 + 1);
    EXPECT_EQ(e0.ts_init, k_t0 + 1);
    const OrderEvent& e1 = r.events[1];
    EXPECT_EQ(e1.type, OrderEventType::Filled);
    EXPECT_EQ(e1.price, px(102));
    EXPECT_EQ(e1.qty, lots(3));
    EXPECT_EQ(e1.leaves_qty, lots(0));
    EXPECT_EQ(e1.exec_id.view(), "S-2");

    ASSERT_EQ(r.trades.size(), 2U);
    EXPECT_EQ(r.trades[0].price, px(101));
    EXPECT_EQ(r.trades[0].qty, lots(5));
    EXPECT_EQ(r.trades[0].aggressor, Side::Buy);
    EXPECT_EQ(r.trades[1].price, px(102));
    EXPECT_EQ(r.trades[1].qty, lots(3));
    EXPECT_TRUE(r.bbo_changed);

    const auto snap = eng.bbo(k_inst);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->ask, px(102));
    EXPECT_EQ(snap->ask_qty, lots(2));
}

TEST(Engine, ClientLimitPartialFillRests) {
    Engine eng;
    SimResult scenery;
    eng.place_background_limit(k_inst, Side::Sell, px(101), lots(5), k_t0, scenery);
    const auto r = eng.handle(make_limit(1, Side::Buy, px(101), lots(8)), k_t0 + 1);
    ASSERT_EQ(r.events.size(), 2U);
    EXPECT_EQ(r.events[0].type, OrderEventType::Accepted);
    EXPECT_EQ(r.events[0].qty, lots(8));
    EXPECT_EQ(r.events[0].leaves_qty, lots(8)); // full qty at ack time
    EXPECT_EQ(r.events[1].type, OrderEventType::Filled);
    EXPECT_EQ(r.events[1].qty, lots(5));
    EXPECT_EQ(r.events[1].leaves_qty, lots(3));
    ASSERT_EQ(r.trades.size(), 1U);
    const auto snap = eng.bbo(k_inst);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(101));
    EXPECT_EQ(snap->bid_qty, lots(3));
    EXPECT_TRUE(snap->ask.is_undef());
}

TEST(Engine, RestingClientHitByBackgroundMarketGetsMakerFill) {
    Engine eng;
    const auto rested = eng.handle(make_limit(1, Side::Sell, px(105), lots(5)), k_t0);
    ASSERT_EQ(rested.events.size(), 1U);
    EXPECT_EQ(rested.events[0].type, OrderEventType::Accepted);
    const ExchangeOrderId order_id = rested.events[0].exchange_order_id;

    SimResult bg;
    eng.place_background_market(k_inst, Side::Buy, lots(3), k_t0 + 2, bg);
    ASSERT_EQ(bg.events.size(), 1U);
    const OrderEvent& maker = bg.events[0];
    EXPECT_EQ(maker.type, OrderEventType::Filled);
    EXPECT_EQ(maker.liquidity, Liquidity::Maker);
    EXPECT_EQ(maker.side, Side::Sell);
    EXPECT_EQ(maker.client_order_id, ClientOrderId::make(1, 1));
    EXPECT_EQ(maker.exchange_client_order_id, test_ulid(1));
    EXPECT_EQ(maker.exchange_order_id, order_id);
    EXPECT_EQ(maker.price, px(105));
    EXPECT_EQ(maker.qty, lots(3));
    EXPECT_EQ(maker.leaves_qty, lots(2));
    EXPECT_EQ(maker.ts_event, k_t0 + 2);
    ASSERT_EQ(bg.trades.size(), 1U);
    EXPECT_EQ(bg.trades[0].price, px(105));
    EXPECT_EQ(bg.trades[0].qty, lots(3));
    EXPECT_EQ(bg.trades[0].aggressor, Side::Buy);
    EXPECT_TRUE(bg.bbo_changed); // best ask qty shrank
}

TEST(Engine, PostOnlyCrossRejected) {
    Engine eng;
    SimResult scenery;
    eng.place_background_limit(k_inst, Side::Sell, px(101), lots(5), k_t0, scenery);
    const auto before = eng.bbo(k_inst);
    const auto r = eng.handle(
        make_limit(1, Side::Buy, px(101), lots(5), TimeInForce::Gtc, OrderFlags::PostOnly),
        k_t0 + 1);
    // a post-only order that would cross is refused
    ASSERT_EQ(r.events.size(), 1U);
    EXPECT_EQ(r.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(r.events[0].leaves_qty, lots(5));
    EXPECT_TRUE(r.trades.empty());
    EXPECT_FALSE(r.bbo_changed);
    EXPECT_EQ(eng.bbo(k_inst), before); // book untouched
}

TEST(Engine, IocRemainderExpires) {
    Engine eng;
    SimResult scenery;
    eng.place_background_limit(k_inst, Side::Sell, px(101), lots(4), k_t0, scenery);
    const auto r =
        eng.handle(make_limit(1, Side::Buy, px(101), lots(10), TimeInForce::Ioc), k_t0 + 1);
    ASSERT_EQ(r.events.size(), 2U);
    EXPECT_EQ(r.events[0].type, OrderEventType::Filled);
    EXPECT_EQ(r.events[0].qty, lots(4));
    EXPECT_EQ(r.events[0].leaves_qty, lots(6));
    EXPECT_EQ(r.events[1].type, OrderEventType::Expired);
    EXPECT_EQ(r.events[1].leaves_qty, lots(6));
    EXPECT_EQ(r.events[1].qty, lots(10));
    const auto rc =
        eng.handle(make_cancel(2, r.events[0].exchange_order_id, test_ulid(1)), k_t0 + 2);
    ASSERT_EQ(rc.events.size(), 1U);
    EXPECT_EQ(rc.events[0].type, OrderEventType::CancelRejected);
}

TEST(Engine, MarketRemainderExpires) {
    Engine eng;
    SimResult scenery;
    eng.place_background_limit(k_inst, Side::Buy, px(99), lots(4), k_t0, scenery);
    const auto r = eng.handle(make_market(1, Side::Sell, lots(10)), k_t0 + 1);
    ASSERT_EQ(r.events.size(), 2U);
    EXPECT_EQ(r.events[0].type, OrderEventType::Filled);
    EXPECT_EQ(r.events[0].price, px(99));
    EXPECT_EQ(r.events[0].qty, lots(4));
    EXPECT_EQ(r.events[1].type, OrderEventType::Expired);
    EXPECT_EQ(r.events[1].leaves_qty, lots(6));
    ASSERT_EQ(r.trades.size(), 1U);
    EXPECT_EQ(r.trades[0].aggressor, Side::Sell);
}

TEST(Engine, CancelProducesCanceled) {
    Engine eng;
    const auto rested = eng.handle(make_limit(1, Side::Buy, px(99), lots(5)), k_t0);
    ASSERT_EQ(rested.events.size(), 1U);
    const ExchangeOrderId target = rested.events[0].exchange_order_id;

    const auto rc = eng.handle(make_cancel(2, target), k_t0 + 1);
    ASSERT_EQ(rc.events.size(), 1U);
    EXPECT_EQ(rc.events[0].type, OrderEventType::Canceled);
    EXPECT_EQ(rc.events[0].qty, lots(5));
    EXPECT_EQ(rc.events[0].leaves_qty, lots(5));
    EXPECT_EQ(rc.events[0].price, px(99));
    EXPECT_EQ(rc.events[0].exchange_order_id, target);
    EXPECT_TRUE(rc.bbo_changed); // the only bid disappeared
    EXPECT_FALSE(eng.bbo(k_inst).has_value());

    const auto rc2 = eng.handle(make_cancel(3, target), k_t0 + 2);
    ASSERT_EQ(rc2.events.size(), 1U);
    EXPECT_EQ(rc2.events[0].type, OrderEventType::CancelRejected);
}

TEST(Engine, CancelResolvesByExchangeClientOrderId) {
    Engine eng;
    const auto rested = eng.handle(make_limit(1, Side::Buy, px(99), lots(5)), k_t0);
    ASSERT_EQ(rested.events.size(), 1U);
    const auto rc = eng.handle(make_cancel(2, ExchangeOrderId{}, test_ulid(1)), k_t0 + 1);
    ASSERT_EQ(rc.events.size(), 1U);
    EXPECT_EQ(rc.events[0].type, OrderEventType::Canceled);
}

TEST(Engine, AmendProducesUpdated) {
    Engine eng;
    const auto rested = eng.handle(make_limit(1, Side::Buy, px(99), lots(5)), k_t0);
    const ExchangeOrderId target = rested.events[0].exchange_order_id;

    const auto ra = eng.handle(make_amend(2, target, px(98), lots(5)), k_t0 + 1);
    ASSERT_EQ(ra.events.size(), 1U);
    EXPECT_EQ(ra.events[0].type, OrderEventType::Updated);
    EXPECT_EQ(ra.events[0].price, px(98));
    EXPECT_EQ(ra.events[0].leaves_qty, lots(5));
    EXPECT_TRUE(ra.bbo_changed);

    const auto rq = eng.handle(make_amend(3, target, px(98), lots(3)), k_t0 + 2);
    ASSERT_EQ(rq.events.size(), 1U);
    EXPECT_EQ(rq.events[0].type, OrderEventType::Updated);
    EXPECT_EQ(rq.events[0].leaves_qty, lots(3));
    const auto snap = eng.bbo(k_inst);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(98));
    EXPECT_EQ(snap->bid_qty, lots(3));
}

TEST(Engine, AmendToCrossingPriceFills) {
    Engine eng;
    SimResult scenery;
    eng.place_background_limit(k_inst, Side::Sell, px(101), lots(3), k_t0, scenery);
    const auto rested = eng.handle(make_limit(1, Side::Buy, px(99), lots(5)), k_t0);
    const ExchangeOrderId target = rested.events[0].exchange_order_id;

    const auto ra = eng.handle(make_amend(2, target, px(102), lots(5)), k_t0 + 1);
    ASSERT_EQ(ra.events.size(), 2U);
    EXPECT_EQ(ra.events[0].type, OrderEventType::Updated);
    EXPECT_EQ(ra.events[0].price, px(102));
    EXPECT_EQ(ra.events[1].type, OrderEventType::Filled);
    EXPECT_EQ(ra.events[1].liquidity, Liquidity::Taker); // the amend took liquidity
    EXPECT_EQ(ra.events[1].price, px(101));
    EXPECT_EQ(ra.events[1].qty, lots(3));
    EXPECT_EQ(ra.events[1].leaves_qty, lots(2));
    ASSERT_EQ(ra.trades.size(), 1U);
    const auto snap = eng.bbo(k_inst);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bid, px(102));
    EXPECT_EQ(snap->bid_qty, lots(2));
}

TEST(Engine, AmendUnknownOrderRejected) {
    Engine eng;
    const auto r = eng.handle(make_amend(1, *ExchangeOrderId::from("999"), px(100), lots(5)), k_t0);
    ASSERT_EQ(r.events.size(), 1U);
    EXPECT_EQ(r.events[0].type, OrderEventType::ModifyRejected);
}

TEST(Engine, DeepBackgroundQuoteDoesNotFlagBbo) {
    Engine eng;
    SimResult first;
    eng.place_background_limit(k_inst, Side::Buy, px(90), lots(5), k_t0, first);
    EXPECT_TRUE(first.bbo_changed); // first bid IS the bbo
    SimResult second;
    eng.place_background_limit(k_inst, Side::Buy, px(85), lots(5), k_t0, second);
    EXPECT_FALSE(second.bbo_changed); // deeper level, top unchanged
}

TEST(Engine, MdGenPopulatesBookAndPrintsTrades) {
    Engine eng;
    MdGen gen{7};
    gen.add_instrument(k_inst, px(1000));
    std::size_t trades = 0;
    for (int i = 0; i < 200; ++i) {
        const UnixNanos now = k_t0 + static_cast<std::uint64_t>(i) * 1'000'000ULL;
        const SimResult r = gen.step(eng, k_inst, now);
        trades += r.trades.size();
        const auto snap = eng.bbo(k_inst);
        ASSERT_TRUE(snap.has_value());
        if (!snap->bid.is_undef() && !snap->ask.is_undef()) {
            ASSERT_LT(snap->bid.raw, snap->ask.raw);
        }
    }
    EXPECT_GT(trades, 0U);
    const auto snap = eng.bbo(k_inst);
    ASSERT_TRUE(snap.has_value());
    EXPECT_FALSE(snap->bid.is_undef());
    EXPECT_FALSE(snap->ask.is_undef());
}

TEST(Engine, DeterminismSameSeedSameScript) {
    const auto run = [] {
        std::string blob;
        Engine eng;
        MdGen gen{42};
        gen.add_instrument(k_inst, px(1000));
        for (int i = 0; i < 500; ++i) {
            const UnixNanos now = k_t0 + static_cast<std::uint64_t>(i) * 1'000'000ULL;
            const SimResult r = gen.step(eng, k_inst, now);
            serialize(r, blob);
            if (i % 50 == 10) {
                const auto snap = eng.bbo(k_inst);
                if (snap.has_value() && !snap->bid.is_undef()) {
                    const auto cr = eng.handle(
                        make_limit(
                            1000 + static_cast<std::uint64_t>(i), Side::Buy, snap->bid, lots(2)),
                        now);
                    serialize(cr, blob);
                }
            }
            if (i % 25 == 24) {
                const Side side = (i / 25) % 2 == 0 ? Side::Buy : Side::Sell;
                const auto cr = eng.handle(
                    make_market(2000 + static_cast<std::uint64_t>(i), side, lots(3)), now);
                serialize(cr, blob);
            }
        }
        const auto snap = eng.bbo(k_inst);
        if (snap.has_value()) {
            append_pod(blob, *snap);
        }
        return blob;
    };
    const std::string a = run();
    const std::string b = run();
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(a, b); // byte-identical event/trade streams and final book top
}

} // namespace
} // namespace raikiri::simcore
