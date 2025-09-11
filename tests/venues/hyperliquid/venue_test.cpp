#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/sign.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::hyperliquid {
namespace {

// l2Book carries this many levels a side and there is no parameter that widens
// it: nSigFigs only coarsens the price grid, and /info answers the same 20.
constexpr std::uint32_t k_hyperliquid_levels = 20;

// One test target covers this adapter, so the fixtures are located relative to
// this file rather than through a compile definition.
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Builds the table the mappers resolve coins against, straight from the real
// catalogue — so every mapping test below divides by steps that were DERIVED
// from the venue's own szDecimals rather than by numbers chosen to make the
// test pass.
exec::InstrumentTable loaded_table() {
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_meta.json");
    const auto assets = parse_meta(body, *storage);
    EXPECT_TRUE(assets.has_value());
    exec::InstrumentTable table;
    for (const auto& asset : *assets) {
        if (const auto instrument = map_instrument(asset, MarketType::Perp);
            instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(HyperliquidVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Hyperliquid);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(HyperliquidVenue, OrderEntryWithoutAKeyRefusesThroughTheSink) {
    // This venue trades now, but a gateway configured with no private key
    // still cannot sign. The refusal has to arrive as an EVENT: nothing is
    // pushed for a request that never left, so an adapter that only returned
    // an error would leave the order in doubt forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.qty = QtyLots::from_raw(5);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].client_order_id.raw, cmd.client_order_id.raw);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0) << "a create that never landed leaves nothing";

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 5) << "a refused cancel leaves the order as it was";

    cmd.kind = CmdKind::Amend;
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);

    // Without an address there is nothing to sign FOR either, so starting the
    // execution side is refused rather than half-working.
    EXPECT_FALSE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());
}

TEST(HyperliquidVenue, ReconciliationPullsRefuseRatherThanReportFlat) {
    // An empty vector reads as "the account holds nothing of ours", and a
    // reconciler acting on that concludes we are flat when we are not.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto market : {MarketType::Spot, MarketType::Perp}) {
        const auto fills = adapter->execution->fill_reports(market, UnixNanos{0});
        ASSERT_FALSE(fills.has_value());
        EXPECT_EQ(fills.error().cls, exec::ErrorClass::AuthFailure);
        const auto open = adapter->execution->open_order_reports(market);
        ASSERT_FALSE(open.has_value());
        EXPECT_EQ(open.error().cls, exec::ErrorClass::AuthFailure);
        const auto one = adapter->execution->query_order(
            *ExchangeClientOrderId::from("00000000000000000000000001"), market);
        ASSERT_FALSE(one.has_value());
        EXPECT_EQ(one.error().cls, exec::ErrorClass::AuthFailure);
    }

    const auto perp = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(perp.has_value());
    EXPECT_EQ(perp.error().cls, exec::ErrorClass::AuthFailure);

    // Spot has no position concept on this venue at all: one HYPE holding
    // backs @107 and every other pair quoting it, and the response carries no
    // entry price. That refusal is about the VENUE, so it survives a gateway
    // that is otherwise fully configured — and it must not be crowded out by
    // the config complaint above.
    const auto spot = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().message.view(), "hyperliquid spot position pull");
}

TEST(HyperliquidVenue, UnservedMarketsRefuseBeforeAnythingElseIsConsulted) {
    // The refusal must name the MARKET. Reaching the instrument table first
    // would answer UnknownInstrument and blame a stale catalogue for a market
    // this adapter was never going to serve.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto& [market, message] :
         {std::pair{MarketType::Option, std::string_view{"hyperliquid options order entry"}},
          std::pair{MarketType::Future,
                    std::string_view{"hyperliquid dated futures order entry"}}}) {
        const auto positions = adapter->execution->position_reports(market);
        ASSERT_FALSE(positions.has_value());
        EXPECT_EQ(positions.error().message.view(), message);
        const auto fills = adapter->execution->fill_reports(market, UnixNanos{0});
        ASSERT_FALSE(fills.has_value());
        EXPECT_EQ(fills.error().message.view(), message);
        const auto open = adapter->execution->open_order_reports(market);
        ASSERT_FALSE(open.has_value());
        EXPECT_EQ(open.error().message.view(), message);
    }

    // And the catalogue refuses them without opening a socket.
    EXPECT_FALSE(adapter->instruments->fetch_all(MarketType::Option).has_value());
    EXPECT_EQ(adapter->instruments->fetch_all(MarketType::Future).error().message.view(),
              "hyperliquid dated futures catalogue");
}

// ------------------------------------------------------------- wire ----

TEST(HyperliquidWire, EventDispatchResolvesKindFromChannel) {
    for (const auto& [file, kind] : {std::pair{"ws_bbo.json", EventKind::Bbo},
                                     std::pair{"ws_trades.json", EventKind::Trade},
                                     std::pair{"ws_l2book_snapshot.json", EventKind::Book},
                                     std::pair{"ws_active_asset_ctx.json", EventKind::AssetCtx},
                                     std::pair{"ws_subscribe_ack.json", EventKind::SubscribeAck},
                                     std::pair{"ws_pong.json", EventKind::Pong},
                                     std::pair{"ws_error.json", EventKind::Error}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(*event, kind) << file;
    }
}

TEST(HyperliquidWire, UnimplementedChannelIsIgnoredNotFatal) {
    // allMids is a real channel this adapter does not subscribe to. Reaching
    // the dispatcher it must resolve to Unknown and be dropped — a venue that
    // adds a channel must not be able to take the gateway down.
    const auto frame = fixture("ws_all_mids.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(*event, EventKind::Unknown);
}

TEST(HyperliquidWire, ControlFramesAreNotMistakenForData) {
    // Reading a refusal as an ack turns "the venue rejected this channel" into
    // "this channel is quiet", which is indistinguishable from a market with
    // no activity — and here it also hides that the socket is about to close.
    const auto bad = fixture("ws_error.json");
    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    EXPECT_FALSE(err->msg.empty());
    // Prose, not a code: there is no numeric error envelope on this venue's
    // websocket at all, unlike okx's 60018.
    EXPECT_NE(err->msg.find("Error parsing JSON"), std::string_view::npos);

    // The acknowledgement echoes the subscription rather than carrying a
    // status field, so nothing about it can be read as a failure.
    const auto ack = fixture("ws_subscribe_ack.json");
    EXPECT_FALSE(parse_stream_error(ack).has_value());
}

TEST(HyperliquidWire, NonJsonFrameIsRejected) {
    EXPECT_FALSE(parse_event("pong").has_value());
    EXPECT_FALSE(parse_event("").has_value());
}

TEST(HyperliquidWire, BboParsesBothSides) {
    const auto frame = fixture("ws_bbo.json");
    const auto bbo = parse_bbo(frame);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->coin, "BTC");
    EXPECT_EQ(bbo->bid_price, "64143.0");
    EXPECT_EQ(bbo->ask_price, "64144.0");
    EXPECT_FALSE(bbo->bid_qty.empty());
    EXPECT_FALSE(bbo->ask_qty.empty());
    // Milliseconds out of a JSON NUMBER, not a digit string. Running this
    // through the decimal parser would scale it by 1e9 and overflow int64.
    EXPECT_GT(bbo->ts_ms, 1'700'000'000'000U);
    EXPECT_LT(bbo->ts_ms, 4'000'000'000'000U);

    // Views point into the caller's buffer, not into parser scratch: the raw
    // token is rebased, and get_string()'s unescape buffer never could be.
    EXPECT_GE(bbo->bid_price.data(), frame.data());
    EXPECT_LE(bbo->bid_price.data() + bbo->bid_price.size(), frame.data() + frame.size());
}

TEST(HyperliquidWire, TradesCarryTakerSideDirectly) {
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trades.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    const auto& trade = trades->front();
    EXPECT_EQ(trade.coin, "BTC");
    EXPECT_FALSE(trade.price.empty());
    EXPECT_FALSE(trade.qty.empty());
    EXPECT_GT(trade.ts_ms, 1'700'000'000'000U);
    // `tid` is a JSON number kept as its raw digits — no quotes to strip.
    EXPECT_FALSE(trade.trade_id.empty());
    EXPECT_EQ(trade.trade_id.find('"'), std::string_view::npos);
    // "A" in the captured frame, and it already means the AGGRESSOR sold: this
    // print landed on the bid (64143.0) that the bbo capture taken on the same
    // second shows. No inversion, unlike binance's buyer-was-maker flag.
    EXPECT_FALSE(trade.aggressor_bought);
}

TEST(HyperliquidWire, OneFrameCanCarrySeveralPrints) {
    // A taker sweeping several makers arrives as one frame; dropping the tail
    // would lose real volume on a busy contract.
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trades_batch.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    EXPECT_EQ(trades->size(), 2U);
    EXPECT_EQ((*trades)[0].ts_ms, (*trades)[1].ts_ms);
}

TEST(HyperliquidWire, L2BookIsSnapshotOnlyWithNoContinuityChain) {
    // The fact this adapter's book handling rests on. Two CONSECUTIVE frames
    // off one socket: both carry a full 20 levels a side and a fresh timestamp
    // and neither carries a sequence id, a previous-sequence id or an action
    // field. There is nothing to chain, so there is no resync path to get
    // wrong — the newest frame simply replaces the book.
    auto storage = std::make_unique<LevelBuffer>();
    const auto first_frame = fixture("ws_l2book_snapshot.json");
    const auto first = parse_book(first_frame, *storage);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->coin, "BTC");
    EXPECT_EQ(first->bids.size(), 20U);
    EXPECT_EQ(first->asks.size(), 20U);
    const auto first_ts = first->ts_ms;
    const std::string first_top_bid{first->bids.front().price};

    auto next_storage = std::make_unique<LevelBuffer>();
    const auto next_frame = fixture("ws_l2book_next.json");
    const auto next = parse_book(next_frame, *next_storage);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->bids.size(), 20U);
    EXPECT_EQ(next->asks.size(), 20U);
    EXPECT_GT(next->ts_ms, first_ts);
    // Both frames restate the whole top of book rather than a delta against
    // the previous one.
    EXPECT_EQ(next->bids.front().price, first_top_bid);

    // levels is positional [bids, asks]: bids descend, asks ascend, and
    // reading them the other way round inverts every spread.
    EXPECT_GT(first->bids.front().price, first->bids.back().price);
    EXPECT_LT(first->asks.front().price, first->asks.back().price);
    EXPECT_LT(first->bids.front().price, first->asks.front().price);
}

TEST(HyperliquidWire, AssetCtxCarriesFundingAndMarkTogether) {
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto ctx = parse_asset_ctx(frame);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->coin, "BTC");
    EXPECT_FALSE(ctx->funding.empty());
    EXPECT_FALSE(ctx->mark_price.empty());
    EXPECT_FALSE(ctx->oracle_price.empty());
    EXPECT_FALSE(ctx->open_interest.empty());
    // One channel answers what okx splits across mark-price and funding-rate.
    EXPECT_NE(ctx->mark_price, ctx->oracle_price);
}

// ------------------------------------------------------------- rest ----

TEST(HyperliquidRest, InfoReadsArePostsWithABody) {
    // The shape this venue does not share with any CEX adapter here: there is
    // no GET endpoint for market data, so the target is constant and the
    // request type lives in the body.
    const auto meta = RequestBuilder::meta();
    EXPECT_EQ(meta.method, net::HttpMethod::Post);
    EXPECT_EQ(meta.target, "/info");
    EXPECT_EQ(meta.body, R"({"type":"meta"})");
    // Omitting this header earns HTTP 415 from the venue, verified live.
    EXPECT_EQ(meta.content_type, "application/json");

    const auto combined = RequestBuilder::meta_and_asset_ctxs();
    EXPECT_EQ(combined.target, meta.target);
    EXPECT_NE(combined.body, meta.body);
}

TEST(HyperliquidRest, MetaParsesUniverseAndFlagsDelisted) {
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_meta.json");
    const auto assets = parse_meta(body, *storage);
    ASSERT_TRUE(assets.has_value());
    ASSERT_EQ(assets->size(), 4U);

    EXPECT_EQ((*assets)[0].name, "BTC");
    EXPECT_EQ((*assets)[0].sz_decimals, 5);
    EXPECT_EQ((*assets)[1].name, "ETH");
    EXPECT_EQ((*assets)[1].sz_decimals, 4);
    // isDelisted is ABSENT on a live row rather than present-and-false.
    EXPECT_FALSE((*assets)[0].delisted);
    EXPECT_TRUE((*assets)[2].delisted);
    // szDecimals of 0 is a real value, not a missing field: XRP sizes are
    // whole coins.
    EXPECT_EQ((*assets)[3].name, "XRP");
    EXPECT_EQ((*assets)[3].sz_decimals, 0);
}

TEST(HyperliquidRest, NullBodyIsRefusedRatherThanReadAsAnEmptyCatalogue) {
    // The venue answers a query it cannot satisfy with HTTP 200 and a body of
    // the bare literal `null`. An empty catalogue and a refused query must not
    // look the same, or a gateway starts up believing the venue lists nothing.
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_null_body.json");
    EXPECT_FALSE(parse_meta(body, *storage).has_value());
    EXPECT_TRUE(is_null_body(body));
    EXPECT_EQ(classify(200, body), exec::ErrorClass::Terminal);
}

TEST(HyperliquidRest, AssetCtxsPairPositionallyWithTheUniverse) {
    // The rows carry no coin at all — they are indexed against
    // meta.universe — so a loader that sorted or filtered either half
    // independently would attach BTC's funding to a different asset silently.
    auto assets_storage = std::make_unique<AssetBuffer>();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto assets = parse_meta(body, *assets_storage);
    ASSERT_FALSE(assets.has_value()) << "the combined body's root is an array, not an object";

    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());
    ASSERT_EQ(ctxs->size(), 4U);
    for (const auto& ctx : *ctxs) {
        EXPECT_TRUE(ctx.coin.empty()) << "REST rows must not claim a coin they do not carry";
        EXPECT_FALSE(ctx.mark_price.empty());
    }
    // The delisted row (index 2, MATIC) ships midPx as JSON null, which must
    // read as absent rather than failing the whole body.
    EXPECT_TRUE((*ctxs)[2].mid_price.empty());
    EXPECT_FALSE((*ctxs)[0].mid_price.empty());

    // The same parser reads the websocket channel's object, so the two
    // transports cannot drift apart.
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto pushed = parse_asset_ctx(frame);
    ASSERT_TRUE(pushed.has_value());
    EXPECT_FALSE(pushed->coin.empty());
}

// -------------------------------------------------------------- map ----

TEST(HyperliquidMap, StepsAreDerivedFromDecimalsAsPowersOfTen) {
    // The venue publishes NO tick or lot size. Both come from szDecimals, in
    // integer fixed point — a double would reintroduce exactly the rounding
    // the domain exists to avoid.
    EXPECT_EQ(step_from_decimals(0)->raw, k_e9);        // 1
    EXPECT_EQ(step_from_decimals(1)->raw, 100'000'000); // 0.1
    EXPECT_EQ(step_from_decimals(5)->raw, 10'000);      // 0.00001
    EXPECT_EQ(step_from_decimals(9)->raw, 1);           // 1e-9, the finest FixedE9 holds
    EXPECT_FALSE(step_from_decimals(10).has_value());
    EXPECT_FALSE(step_from_decimals(-1).has_value());
}

TEST(HyperliquidMap, InstrumentDerivesBothStepsAndSkipsDelisted) {
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_meta.json");
    const auto assets = parse_meta(body, *storage);
    ASSERT_TRUE(assets.has_value());

    // BTC: szDecimals 5 -> lot 1e-5, and price capped at 6-5=1 decimal place,
    // so tick 0.1.
    const auto btc = map_instrument((*assets)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(base_of(*btc).lot_size.raw, 10'000);
    EXPECT_EQ(base_of(*btc).tick_size.raw, 100'000'000);
    EXPECT_EQ(base_of(*btc).exchange_symbol.view(), "BTC");
    // Sizes are base units, not contracts: no ctVal-equivalent to load.
    EXPECT_EQ(base_of(*btc).contract_value.raw, k_e9);
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*btc));
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 3600U);

    // XRP: szDecimals 0 -> lot 1 whole coin, tick 1e-6. The extreme of the
    // derivation, and the one a hardcoded step would get wrong.
    const auto xrp = map_instrument((*assets)[3], MarketType::Perp);
    ASSERT_TRUE(xrp.has_value());
    EXPECT_EQ(base_of(*xrp).lot_size.raw, k_e9);
    EXPECT_EQ(base_of(*xrp).tick_size.raw, 1'000);

    // Delisted keeps its slot in the universe forever and must never reach the
    // table: no book will ever update it.
    EXPECT_FALSE(map_instrument((*assets)[2], MarketType::Perp).has_value());
    // Spot exists but under a different catalogue and naming scheme, so a spot
    // request must not silently load perps.
    EXPECT_FALSE(map_instrument((*assets)[0], MarketType::Spot).has_value());
}

TEST(HyperliquidMap, PublishedPricesAllDivideByTheDerivedTick) {
    // The claim the derivation rests on: every price the venue publishes
    // honours the decimal-place cap, so exact division never drops one. Both
    // markPx values below carry MORE than five significant figures, which is
    // why the sig-fig half of the rule is not modelled as a step.
    const auto table = loaded_table();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    // Named, not a temporary: every view a parser returns points into the
    // caller's buffer, so the body has to outlive what was read out of it.
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());

    auto btc = (*ctxs)[0];
    btc.coin = "BTC";
    const auto btc_funding = map_funding(btc, table, UnixNanos{1});
    ASSERT_TRUE(btc_funding.has_value());
    EXPECT_FALSE(btc_funding->mark_price.is_undef());

    auto xrp = (*ctxs)[3];
    xrp.coin = "XRP";
    const auto xrp_funding = map_funding(xrp, table, UnixNanos{1});
    ASSERT_TRUE(xrp_funding.has_value());
    // 1.0195 over a derived tick of 1e-6.
    EXPECT_EQ(xrp_funding->mark_price.raw, 1'019'500);
}

TEST(HyperliquidMap, PriceOffTheDerivedGridIsDroppedNotRounded) {
    // A value that does not divide evenly means the venue changed an
    // instrument's precision. Rounding it would put a mark price one tick away
    // from the one the venue actually published.
    const auto table = loaded_table();
    WireTrade wire{};
    wire.coin = "BTC";
    wire.qty = "0.01856";
    wire.ts_ms = 1'786'395'066'376ULL;

    wire.price = "64137.0"; // one decimal: on the grid
    EXPECT_TRUE(map_trade(wire, table, UnixNanos{1}).has_value());

    wire.price = "64137.05"; // two decimals: finer than 6 - szDecimals allows
    EXPECT_FALSE(map_trade(wire, table, UnixNanos{1}).has_value());

    wire.price = "64137.0";
    wire.qty = "0.018565"; // six decimals against a lot step of 1e-5
    EXPECT_FALSE(map_trade(wire, table, UnixNanos{1}).has_value());
}

TEST(HyperliquidMap, BboMapsThroughTheDerivedStepsWithVenueTime) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());

    const auto bbo = map_bbo(*wire, table, UnixNanos{7});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 641'430); // 64143.0 / 0.1
    EXPECT_EQ(bbo->ask.raw, 641'440);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    // ts_event is the venue's millisecond, ts_init our ingest nanosecond.
    EXPECT_EQ(bbo->ts_event.ns, wire->ts_ms * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 7U);
}

TEST(HyperliquidMap, UnknownCoinMapsToNothing) {
    // Every mapper resolves the coin against the table first; an asset the
    // catalogue never loaded has no steps to divide by.
    const auto table = loaded_table();
    WireBbo wire{};
    wire.coin = "NOSUCHCOIN";
    wire.bid_price = "1.0";
    wire.bid_qty = "1.0";
    wire.ask_price = "2.0";
    wire.ask_qty = "1.0";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(HyperliquidMap, EmptyBookSideDropsTheTickRatherThanQuotingZero) {
    // The venue sends JSON null for a side with nothing resting, which the
    // parser surfaces as empty views. A zero-priced bid would be a real quote.
    const auto table = loaded_table();
    WireBbo wire{};
    wire.coin = "BTC";
    wire.ask_price = "64144.0";
    wire.ask_qty = "1.0";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(HyperliquidMap, FundingIsHourlyAndCarriesMarkButNoSettlementTime) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto wire = parse_asset_ctx(frame);
    ASSERT_TRUE(wire.has_value());

    const auto funding = map_funding(*wire, table, UnixNanos{11});
    ASSERT_TRUE(funding.has_value());
    // 0.0000125 per HOUR. The same contract on a CEX quotes ~0.0001 per 8h;
    // comparing the two without scaling is a factor of eight.
    EXPECT_EQ(funding->rate.raw, 12'500);
    EXPECT_EQ(funding->interval_s, 3600U);
    // Mark rides the same payload, so unlike okx there is no reason to leave
    // it undefined.
    EXPECT_EQ(funding->mark_price.raw, 641'430);
    // The venue publishes no settlement time and no forecast rate on either
    // transport; filling either in would present a derivation as a venue fact.
    EXPECT_TRUE(funding->next_funding_time.ns == 0U);
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    // No timestamp on this payload at all, so ingest time is the only honest
    // ts_event.
    EXPECT_EQ(funding->ts_event.ns, 11U);
    EXPECT_EQ(funding->ts_init.ns, 11U);
}

TEST(HyperliquidMap, TheDaysVolumeRidesTheSameContextAsFunding) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto wire = parse_asset_ctx(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->day_base_volume, "27652.04587");
    EXPECT_EQ(wire->day_notional_volume, "1786723009.1462507248");

    const auto stats = map_ticker_stats(*wire, table, UnixNanos{11});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 27'652'045'870'000);
    // Truncated at 1e-9, not refused: the venue writes ten fractional digits
    // and an exact read would report a $1.7bn day as no volume at all.
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'786'723'009'146'250'724);
    // Both raws are e9, so their ratio is a price: 64614, between this
    // context's own markPx 64143 and prevDayPx 65105. The names do not say
    // which of Ntl and Base is the coin; this does.
    EXPECT_EQ(stats->volume_24h_quote.raw / stats->volume_24h_base.raw, 64'614);
    // No timestamp on either transport, so ingest is the only honest one.
    EXPECT_EQ(stats->ts_event.ns, 11U);
    EXPECT_EQ(stats->ts_init.ns, 11U);
}

TEST(HyperliquidMap, ARestContextNamesNoCoinAndPublishesNoVolume) {
    // The REST rows pair POSITIONALLY against meta.universe and carry no coin,
    // so there is nothing to attribute their volume to until a loader has done
    // that pairing.
    const auto table = loaded_table();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());
    ASSERT_FALSE(ctxs->empty());
    EXPECT_EQ((*ctxs)[0].day_base_volume, "27654.4941000001");
    EXPECT_FALSE(map_ticker_stats((*ctxs)[0], table, UnixNanos{1}).has_value());

    // Named, it maps — the same row the funding test pairs by hand.
    auto btc = (*ctxs)[0];
    btc.coin = "BTC";
    const auto stats = map_ticker_stats(btc, table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote.raw / stats->volume_24h_base.raw, 64'614);
}

TEST(HyperliquidMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = loaded_table();
    WireAssetCtx wire{};
    wire.coin = "BTC";
    wire.mark_price = "64143.0";
    EXPECT_FALSE(map_funding(wire, table, UnixNanos{1}).has_value());
}

// --------------------------------------------------------------- ws ----

TEST(HyperliquidWs, SubscribeFrameCarriesExactlyOneSubscription) {
    Protocol protocol;
    const auto frame = protocol.subscribe_frame({.channel = "l2Book", .coin = "BTC"});
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}})");

    const auto stop = protocol.unsubscribe_frame({.channel = "l2Book", .coin = "BTC"});
    ASSERT_TRUE(stop.has_value());
    EXPECT_NE(stop->find("unsubscribe"), std::string_view::npos);

    // JSON both ways here, where okx answers a bare "ping" with bare text.
    EXPECT_EQ(Protocol::ping_frame(), R"({"method":"ping"})");
}

TEST(HyperliquidWs, DeclinedKindNeverReachesTheWire) {
    // An empty channel would subscribe to the literal "", which this venue
    // answers by CLOSING the socket — taking every other subscription with it.
    // So the refusal has to happen here rather than being left to the venue.
    Protocol protocol;
    EXPECT_FALSE(protocol.subscribe_frame({.channel = "", .coin = "BTC"}).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({.channel = "bbo", .coin = ""}).has_value());
}

TEST(HyperliquidWs, FundingAndMarkShareOneChannel) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "bbo");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "trades");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "l2Book");
    // One subscription covers both, unlike okx where they are separate
    // channels — the per-asset context carries markPx and funding together.
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "activeAssetCtx");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), channel_for(exec::SubKind::Funding));
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
    EXPECT_EQ(stream_path(), "/ws");
    EXPECT_NE(stream_host(true), stream_host(false));
}

// ----------------------------------------------------------- errors ----

TEST(HyperliquidErrors, StatusClassificationDefaultsToTerminal) {
    // Every status here was observed live against production /info.
    EXPECT_EQ(classify(400, "Failed to parse the request body as JSON"),
              exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(405, ""), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(415, "Expected request with `Content-Type: application/json`"),
              exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(422, "Failed to deserialize the JSON body into the target type"),
              exec::ErrorClass::Terminal);
    // An unrecognised refusal repeated at full speed is how an address earns a
    // ban; a spuriously terminal read is recoverable by asking again later.
    EXPECT_EQ(classify(451, ""), exec::ErrorClass::Terminal);

    // A 2xx reaches here only once its body has already failed to parse, and
    // the body is the only thing that says which failure it was: `null` is a
    // definitive "no such thing", anything else is a read worth repeating.
    EXPECT_EQ(classify(200, "null"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(200, R"({"universe":)"), exec::ErrorClass::Retryable);

    EXPECT_EQ(classify(429, ""), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(500, "null"), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(503, ""), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(403, ""), exec::ErrorClass::AuthFailure);
    // Status 0 is a transport failure, not a venue verdict.
    EXPECT_EQ(classify(0, ""), exec::ErrorClass::Connectivity);
}

TEST(HyperliquidErrors, NullBodyDetectionToleratesWhitespace) {
    EXPECT_TRUE(is_null_body("null"));
    EXPECT_TRUE(is_null_body("  null\n"));
    EXPECT_FALSE(is_null_body(""));
    EXPECT_FALSE(is_null_body(R"({"universe":[]})"));
}

TEST(HyperliquidMap, BookSnapshotCrossesIntoTheDomain) {
    // The venue publishes full snapshots, so the mapper carries no book state:
    // whatever the newest frame says replaces what shm holds.
    const auto table = loaded_table();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_l2book_snapshot.json");
    const auto wire = parse_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_book(*wire, table, UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, k_hyperliquid_levels) << "the whole side the venue sends";
    EXPECT_EQ(book->ask_count, k_hyperliquid_levels);
    static_assert(k_hyperliquid_levels <= k_book_levels);

    // Bids descend, asks ascend, and the two never cross — a book that fails
    // this is not a book, and publishing it would price every strategy off it.
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
}

TEST(HyperliquidMap, AnUnknownCoinYieldsNoBook) {
    const auto table = loaded_table();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_l2book_snapshot.json");
    auto wire = parse_book(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    wire->coin = "NOSUCHCOIN";
    EXPECT_FALSE(map_book(*wire, table, UnixNanos{1}).has_value());
}

// ------------------------------------------------------------ signing ----
//
// Hyperliquid signs orders with a secp256k1 key over EIP-712 typed data, not
// with an HMAC. There is no useful feedback from the venue when any stage of
// that is wrong — a bad signature recovers to SOME address, and the answer is
// "User or API Wallet 0x<a stranger> does not exist", naming a different
// stranger every attempt. So every stage is pinned to a vector published in
// the venue's own SDK test suite rather than to this implementation's output.
//
// Source: hyperliquid-dex/hyperliquid-python-sdk, tests/signing_test.py and
// hyperliquid/utils/signing.py (fetched 2026-08-26).

std::vector<std::uint8_t> unhex(std::string_view text) {
    const auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((value(text[i]) << 4) | value(text[i + 1])));
    }
    return out;
}

std::string hex_of(const Hash& hash) {
    std::string out;
    out.reserve(hash.size() * 2);
    for (const auto byte : hash) {
        out.push_back("0123456789abcdef"[byte >> 4]);
        out.push_back("0123456789abcdef"[byte & 0x0FU]);
    }
    return out;
}

Hash keccak_of(std::string_view text) {
    return keccak256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

// The SDK's own test key, and the one every vector below was produced with.
constexpr std::string_view k_test_key =
    "0x0123456789012345678901234567890123456789012345678901234567890123";

TEST(HyperliquidSign, KeccakIsKeccakAndNotSha3) {
    // The whole signature rests on this: SHA3-256 differs from keccak-256 by
    // one padding byte, produces an equally well-formed digest, and is what
    // OpenSSL ships under the nearest name. These are the canonical Ethereum
    // vectors.
    EXPECT_EQ(hex_of(keccak_of("")),
              "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    EXPECT_EQ(hex_of(keccak_of("abc")),
              "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    EXPECT_EQ(hex_of(keccak_of("testing")),
              "5f16f4c7f149ac4f9510d9cf8cf384038ad348b3bcdc01915f95de12df9d1b02");
    EXPECT_EQ(hex_of(keccak_of("hello")),
              "1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8");

    // A message spanning exactly one rate block and one past it: the padding
    // arm nobody exercises by accident, since 136 bytes is the one length at
    // which a whole extra block of pure padding is absorbed.
    const std::string block(136, 'a');
    EXPECT_NE(hex_of(keccak_of(block)), hex_of(keccak_of(block + "a")));
    EXPECT_EQ(hex_of(keccak_of(block)).size(), 64U);
}

TEST(HyperliquidSign, ActionHashMatchesThePublishedConnectionId) {
    // From test_phantom_agent_creation_matches_production: an ETH IOC buy,
    // asset 4, nonce 1677777606040, no vault. The msgpack bytes are this
    // repo's own encoder (below); the expected hash is the SDK's.
    const auto action = unhex(
        "83a474797065a56f72646572a66f72646572739186a16104a162c3a170a6313637302e31a173a6302e3031"
        "3437a172c2a17481a56c696d697481a3746966a3496f63a867726f7570696e67a26e61");
    const auto hash = action_hash(action, 1677777606040ULL, {}, 0);
    EXPECT_EQ(hex_of(hash), "0fcbeda5ae3c4950a548021552a4fea2226858c4453571bf3f24ba017eac2908");
}

TEST(HyperliquidSign, VaultAndExpiryChangeThePreimageNotJustTheFields) {
    const auto action = unhex("82a474797065a564756d6d79a36e756dcf000000174876e800");
    // An absent vault writes one 0x00 byte; a present one writes 0x01 and then
    // twenty raw address bytes. They are different preimages, not a field
    // toggle, which is why an empty string is not the same as a zero address.
    EXPECT_EQ(hex_of(action_hash(action, 0, {}, 0)),
              "f528daee6a0bd11407b483cfcd9a48c56884180b70ee86f124053e5fc1bf4d57");
    EXPECT_EQ(hex_of(action_hash(action, 0, "0x1719884eb866cb12b2287399b15f7db5e7d775ea", 0)),
              "de9e09a7a3da45cc694096d4bfdcd89bc1b892d05497c5ecc1f56c335945184c");
}

TEST(HyperliquidSign, MainnetAndTestnetDigestsDiffer) {
    // The Agent's `source` — "a" or "b" — is the only thing separating the two
    // networks in the signed bytes. Getting it backwards produces a signature
    // the other network would honour.
    Hash action{};
    action.fill(0x11);
    EXPECT_NE(agent_digest(action, true), agent_digest(action, false));
}

TEST(HyperliquidSign, PublishedSignatureVectorsReproduceExactly) {
    // RFC 6979 makes signing deterministic, so these are equalities rather
    // than "verifies against the public key" — the strongest form of evidence
    // available without a live account.
    struct Vector {
        std::string_view action;
        std::uint64_t nonce;
        std::string_view vault;
        bool mainnet;
        std::string_view r;
        std::string_view s;
        std::uint8_t v;
    };

    constexpr std::string_view k_dummy = "82a474797065a564756d6d79a36e756dcf000000174876e800";
    constexpr std::string_view k_order =
        "83a474797065a56f72646572a66f72646572739186a16101a162c3a170a3313030a173a3313030a172c2a174"
        "81a56c696d697481a3746966a3477463a867726f7570696e67a26e61";
    constexpr std::string_view k_order_cloid =
        "83a474797065a56f72646572a66f72646572739187a16101a162c3a170a3313030a173a3313030a172c2a174"
        "81a56c696d697481a3746966a3477463a163d92230783030303030303030303030303030303030303030303"
        "030303030303030303031a867726f7570696e67a26e61";
    constexpr std::string_view k_trigger =
        "83a474797065a56f72646572a66f72646572739186a16101a162c3a170a3313030a173a3313030a172c2a174"
        "81a77472696767657283a869734d61726b6574c3a9747269676765725078a3313033a47470736ca2736ca867"
        "726f7570696e67a26e61";

    const std::array<Vector, 8> vectors{
        Vector{k_dummy,
               0,
               {},
               true,
               "0x53749d5b30552aeb2fca34b530185976545bb22d0b3ce6f62e31be961a59298",
               "0x755c40ba9bf05223521753995abb2f73ab3229be8ec921f350cb447e384d8ed8",
               27},
        Vector{k_dummy,
               0,
               {},
               false,
               "0x542af61ef1f429707e3c76c5293c80d01f74ef853e34b76efffcb57e574f9510",
               "0x17b8b32f086e8cdede991f1e2c529f5dd5297cbe8128500e00cbaf766204a613",
               28},
        Vector{k_order,
               0,
               {},
               true,
               "0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d2806b100d78e",
               "0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a3162b6ea940e",
               28},
        Vector{k_order,
               0,
               {},
               false,
               "0x82b2ba28e76b3d761093aaded1b1cdad4960b3af30212b343fb2e6cdfa4e3d54",
               "0x6b53878fc99d26047f4d7e8c90eb98955a109f44209163f52d8dc4278cbbd9f5",
               27},
        Vector{k_order_cloid,
               0,
               {},
               true,
               "0x41ae18e8239a56cacbc5dad94d45d0b747e5da11ad564077fcac71277a946e3",
               "0x3c61f667e747404fe7eea8f90ab0e76cc12ce60270438b2058324681a00116da",
               27},
        Vector{k_dummy,
               0,
               "0x1719884eb866cb12b2287399b15f7db5e7d775ea",
               true,
               "0x3c548db75e479f8012acf3000ca3a6b05606bc2ec0c29c50c515066a326239",
               "0x4d402be7396ce74fbba3795769cda45aec00dc3125a984f2a9f23177b190da2c",
               28},
        Vector{k_trigger,
               0,
               {},
               true,
               "0x98343f2b5ae8e26bb2587daad3863bc70d8792b09af1841b6fdd530a2065a3f9",
               "0x6b5bb6bb0633b710aa22b721dd9dee6d083646a5f8e581a20b545be6c1feb405",
               27},
        Vector{k_trigger,
               0,
               {},
               false,
               "0x971c554d917c44e0e1b6cc45d8f9404f32172a9d3b3566262347d0302896a2e4",
               "0x206257b104788f80450f8e786c329daa589aa0b32ba96948201ae556d5637eac",
               28},
    };

    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        const auto& entry = vectors[i];
        const auto hash = action_hash(unhex(entry.action), entry.nonce, entry.vault, 0);
        Signature signature{};
        ASSERT_TRUE(wallet.sign(agent_digest(hash, entry.mainnet), signature)) << i;
        EXPECT_EQ(signature.r.view(), entry.r) << i;
        EXPECT_EQ(signature.s.view(), entry.s) << i;
        EXPECT_EQ(signature.v, entry.v) << i;
    }
}

TEST(HyperliquidSign, AddressComparisonIgnoresTheChecksumCase) {
    // The account reads name an address and the writes prove a key; nothing on
    // the wire ties the two together, so the gateway compares them itself. An
    // Ethereum address is written in MIXED CASE as a checksum, and a byte
    // comparison would report a correctly configured gateway as broken.
    EXPECT_TRUE(same_address("0x5e9ee1089755c3435139848e47e6635505d5a13a",
                             "0x5E9EE1089755C3435139848E47E6635505D5A13A"));
    EXPECT_FALSE(same_address("0x5e9ee1089755c3435139848e47e6635505d5a13a",
                              "0x5e9ee1089755c3435139848e47e6635505d5a13b"));
    EXPECT_FALSE(same_address("", ""));
    EXPECT_FALSE(same_address("0x5e9ee1", "0x5e9ee1089755c3435139848e47e6635505d5a13a"));

    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    EXPECT_TRUE(same_address(wallet.address(), wallet.address()));
    // The address is derived from the public key, so it is stable for a key and
    // cannot collide with another one.
    Wallet other;
    ASSERT_TRUE(other.load("0x0123456789012345678901234567890123456789012345678901234567890124"));
    EXPECT_FALSE(same_address(wallet.address(), other.address()));
}

TEST(HyperliquidSign, KeyMaterialIsValidatedRatherThanTrusted) {
    Wallet wallet;
    EXPECT_FALSE(wallet.load(""));
    EXPECT_FALSE(wallet.load("0x00"));
    EXPECT_FALSE(wallet.load(std::string(64, 'z')));
    // Zero and the group order are not keys: both would sign to something
    // well-formed rather than failing.
    EXPECT_FALSE(wallet.load(std::string(64, '0')));
    EXPECT_FALSE(wallet.load("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141"));
    EXPECT_FALSE(wallet.loaded());

    ASSERT_TRUE(wallet.load(k_test_key));
    // Same key without the prefix every Ethereum tool writes.
    Wallet bare;
    ASSERT_TRUE(bare.load(k_test_key.substr(2)));
    EXPECT_EQ(bare.address(), wallet.address());
    EXPECT_EQ(wallet.address().size(), 42U);
    EXPECT_TRUE(wallet.address().starts_with("0x"));
}

// ------------------------------------------------------- account reads ----

// Both markets in one table, which is how the gateway holds them: /info answers
// perps and spot pairs in the SAME openOrders and userFills arrays, so a
// one-market table cannot tell "not ours" from "not loaded".
exec::InstrumentTable loaded_both() {
    auto table = loaded_table();
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto body = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(body, *storage);
    EXPECT_TRUE(pairs.has_value());
    for (const auto& pair : *pairs) {
        if (const auto instrument = map_spot_instrument(pair); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

TEST(HyperliquidWire, PerpAssetIdsAreWirePositionsNotTableSlots) {
    // MATIC is delisted and keeps its slot forever, so the fourth row of the
    // universe is asset 3 while it is only the third instrument this adapter
    // loads. Deriving the id by counting kept rows sends every XRP order to
    // the asset above it.
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_meta.json");
    const auto assets = parse_meta(body, *storage);
    ASSERT_TRUE(assets.has_value());
    ASSERT_EQ(assets->size(), 4U);
    EXPECT_EQ((*assets)[3].name, "XRP");
    EXPECT_EQ((*assets)[3].index, 3);
    EXPECT_TRUE((*assets)[2].delisted);

    const auto table = loaded_table();
    const auto* xrp = table.find("XRP");
    ASSERT_NE(xrp, nullptr);
    EXPECT_EQ(base_of(*xrp).id.symbol().raw, 2U) << "the slot, which is NOT the asset id";
}

TEST(HyperliquidMap, SpotPairsLoadFromTheirBaseTokenPrecision) {
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto body = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(body, *storage);
    ASSERT_TRUE(pairs.has_value());
    ASSERT_EQ(pairs->size(), 3U);

    // The pair row names its tokens by index and carries no precision of its
    // own; szDecimals lives on the base TOKEN.
    EXPECT_EQ((*pairs)[1].name, "@107");
    EXPECT_EQ((*pairs)[1].base, "HYPE");
    EXPECT_EQ((*pairs)[1].index, 107);
    EXPECT_EQ((*pairs)[1].sz_decimals, 2);

    const auto hype = map_spot_instrument((*pairs)[1]);
    ASSERT_TRUE(hype.has_value());
    const auto& base = base_of(*hype);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    // Spot prices carry 8 - szDecimals places, two more than a perp's 6.
    EXPECT_EQ(base.tick_size.raw, parse_decimal_e9("0.000001")->raw);
    EXPECT_EQ(base.lot_size.raw, parse_decimal_e9("0.01")->raw);
    // The wire coin is the key; the readable name is built from the token.
    EXPECT_EQ(base.exchange_symbol.view(), "@107");
    EXPECT_EQ(base.local_symbol.view(), "HYPE/USDC");

    // The canonical pair is the one exception: it is spelled by name on the
    // wire, and its base token quotes whole units.
    const auto purr = map_spot_instrument((*pairs)[0]);
    ASSERT_TRUE(purr.has_value());
    EXPECT_EQ(base_of(*purr).exchange_symbol.view(), "PURR/USDC");
    EXPECT_EQ(base_of(*purr).lot_size.raw, parse_decimal_e9("1")->raw);
    EXPECT_EQ(base_of(*purr).tick_size.raw, parse_decimal_e9("0.00000001")->raw);
}

TEST(HyperliquidMap, SpotAssetIdsSitTenThousandAboveThePerpSpace) {
    // Asset 107 is a perpetual and asset 10107 is HYPE/USDC. Nothing fails if
    // this is confused — it trades the other market.
    EXPECT_EQ(spot_asset_id(0), 10'000);
    EXPECT_EQ(spot_asset_id(107), 10'107);
    EXPECT_EQ(spot_asset_id(334), 10'334);
}

TEST(HyperliquidWire, PositionsAreSignedAndShareOneSnapshotInstant) {
    auto storage = std::make_unique<PositionBuffer>();
    const auto body = fixture("rest_clearinghouse_state.json");
    const auto positions = parse_positions(body, *storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 4U);
    EXPECT_EQ((*positions)[0].coin, "BTC");
    // `szi` is signed and there is no side field beside it to disagree with.
    EXPECT_EQ((*positions)[0].size, "-268.52681");
    EXPECT_EQ((*positions)[0].entry_px, "74044.9");
    // One `time` for the whole snapshot; no row carries its own.
    EXPECT_EQ(storage->ts_ms, 1787746850812ULL);
}

TEST(HyperliquidMap, PositionsKeepTheirSignAndDropWhatIsNotLoaded) {
    const auto table = loaded_table();
    auto storage = std::make_unique<PositionBuffer>();
    const auto body = fixture("rest_clearinghouse_state.json");
    const auto positions = parse_positions(body, *storage);
    ASSERT_TRUE(positions.has_value());

    std::vector<exec::PositionReport> reports;
    const auto ts = ms_to_unix(storage->ts_ms, UnixNanos{});
    for (const auto& wire : *positions) {
        if (auto report = map_position(wire, table, ts); report.has_value()) {
            reports.push_back(*report);
        }
    }
    // FARTCOIN is a live perp this trimmed catalogue does not carry, so it maps
    // to nothing rather than to a neighbour's steps.
    ASSERT_EQ(reports.size(), 3U);
    EXPECT_LT(reports[0].net_qty.raw, 0) << "a short must not arrive as a long";
    EXPECT_EQ(reports[0].net_qty.raw, -26'852'681); // -268.52681 / 1e-5
    EXPECT_EQ(reports[0].avg_entry.raw, 740'449);   // 74044.9 / 0.1
    EXPECT_EQ(reports[2].net_qty.raw, -3'082'681);  // XRP trades whole coins
    EXPECT_EQ(reports[0].ts_venue.ns, 1787746850812ULL * 1'000'000ULL);
}

TEST(HyperliquidMap, PositionSizesSurviveTheVenuesFloatSerialiser) {
    // A live response carries "-4216984.4000000004" for a position whose lot is
    // 0.1: the venue serialises `szi` from a float. Digits past 1e-9 are below
    // every step this domain can express, so they are dropped — but a size
    // genuinely off the step is still refused rather than rounded onto it.
    const auto table = loaded_table();
    const auto* eth = table.find("ETH");
    ASSERT_NE(eth, nullptr);
    const auto ts = UnixNanos{7};

    const auto noisy = map_position(
        {.coin = "ETH", .size = "-24856.6978000000004", .entry_px = "1981.64"}, table, ts);
    ASSERT_TRUE(noisy.has_value());
    EXPECT_EQ(noisy->net_qty.raw, -248'566'978);

    // Half a lot is not dust: ETH steps by 1e-4.
    EXPECT_FALSE(map_position({.coin = "ETH", .size = "1.00005", .entry_px = "1981.64"}, table, ts)
                     .has_value());
}

TEST(HyperliquidWire, OpenOrdersCarryBothMarketsInOneAnswer) {
    auto storage = std::make_unique<OpenOrderBuffer>();
    const auto body = fixture("rest_open_orders.json");
    const auto orders = parse_open_orders(body, *storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 4U);
    EXPECT_EQ((*orders)[0].coin, "ETH");  // a perpetual
    EXPECT_EQ((*orders)[1].coin, "@334"); // a spot pair, by index
    EXPECT_TRUE((*orders)[0].buy);
    EXPECT_FALSE((*orders)[1].buy);
    // An order placed without one carries no `cloid` key at all.
    EXPECT_EQ((*orders)[0].cloid, "0x00000000000000000000000325566338");
    EXPECT_TRUE((*orders)[1].cloid.empty());
    // The plain form says nothing about post-only or triggers.
    EXPECT_TRUE((*orders)[0].tif.empty());
}

TEST(HyperliquidWire, OnlyTheFrontendFormRevealsPostOnly) {
    auto storage = std::make_unique<OpenOrderBuffer>();
    const auto body = fixture("rest_frontend_open_orders.json");
    const auto orders = parse_open_orders(body, *storage);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 4U);
    EXPECT_EQ((*orders)[0].tif, "Alo") << "the only place post-only appears";
    EXPECT_EQ((*orders)[1].tif, "Gtc");
    EXPECT_EQ((*orders)[0].order_type, "Limit");
    EXPECT_FALSE((*orders)[0].is_trigger);
    // A JSON null cloid reads as absent, not as the four letters.
    EXPECT_TRUE((*orders)[1].cloid.empty());
}

TEST(HyperliquidMap, OpenOrdersResolveIntoTheirOwnMarket) {
    const auto table = loaded_both();
    auto storage = std::make_unique<OpenOrderBuffer>();
    const auto body = fixture("rest_frontend_open_orders.json");
    const auto orders = parse_open_orders(body, *storage);
    ASSERT_TRUE(orders.has_value());

    std::vector<exec::OrderStatusReport> perp;
    std::vector<exec::OrderStatusReport> spot;
    for (const auto& wire : *orders) {
        const auto report = map_open_order(wire, table);
        if (!report.has_value()) {
            continue;
        }
        (report->instrument.market() == MarketType::Perp ? perp : spot).push_back(*report);
    }
    ASSERT_EQ(perp.size(), 1U);
    ASSERT_EQ(spot.size(), 3U);

    EXPECT_EQ(perp[0].side, Side::Buy);
    EXPECT_EQ(perp[0].status, OrderStatus::Accepted) << "sz equals origSz: untouched";
    EXPECT_EQ(perp[0].price.raw, 246'150); // 2461.5 / 0.01
    EXPECT_EQ(perp[0].qty.raw, 89'900);    // 8.99 / 1e-4
    EXPECT_EQ(perp[0].leaves.raw, 89'900);
    EXPECT_EQ(perp[0].exchange_order_id.view(), "527278286739");
    // The cloid is our own ULID in another base, so it comes back whole.
    EXPECT_EQ(perp[0].exchange_client_order_id.view().size(), 26U);

    // HYPE/USDC steps by 1e-6 in price and 0.01 in size.
    EXPECT_EQ(spot[2].price.raw, 34'624'000);
    EXPECT_EQ(spot[2].qty.raw, 3'037);
}

TEST(HyperliquidMap, PartialFillShowsAsLeavesBelowTheOriginal) {
    const auto table = loaded_table();
    const auto report = map_open_order(
        {.coin = "ETH", .price = "2461.5", .qty = "4.0", .orig_qty = "8.99", .oid = 1, .buy = true},
        table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->qty.raw, 89'900);
    EXPECT_EQ(report->leaves.raw, 40'000);
}

TEST(HyperliquidMap, ADormantTriggerOrderIsNotReportedAsResting) {
    // `limitPx` on a trigger order is where it WILL rest, not where it rests.
    // Reporting it would tell a reconciler about an order not in the book.
    const auto table = loaded_table();
    EXPECT_FALSE(map_open_order({.coin = "ETH",
                                 .price = "2461.5",
                                 .qty = "8.99",
                                 .orig_qty = "8.99",
                                 .oid = 1,
                                 .buy = true,
                                 .is_trigger = true},
                                table)
                     .has_value());
}

TEST(HyperliquidWire, OrderStatusSeparatesForgottenFromRefused) {
    const auto known = fixture("rest_order_status.json");
    const auto found = parse_order_status(known);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->found);
    EXPECT_EQ(found->status, "canceled");
    EXPECT_EQ(found->ts_ms, 1787746854240ULL);
    EXPECT_EQ(found->order.coin, "ETH");

    // An id the venue never minted and one it has forgotten answer identically,
    // and neither is an error.
    const auto missing = fixture("rest_order_status_unknown.json");
    const auto unknown = parse_order_status(missing);
    ASSERT_TRUE(unknown.has_value());
    EXPECT_FALSE(unknown->found);
}

TEST(HyperliquidMap, ATerminalStatusLeavesNothingResting) {
    const auto table = loaded_table();
    const auto body = fixture("rest_order_status.json");
    const auto wire = parse_order_status(body);
    ASSERT_TRUE(wire.has_value());

    const auto report = map_order_status(*wire, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Canceled);
    // Leaving `leaves` at the size the venue last saw would tell a reconciler
    // there is still work in the book.
    EXPECT_EQ(report->leaves.raw, 0);
    EXPECT_EQ(report->ts_venue.ns, 1787746854240ULL * 1'000'000ULL);

    EXPECT_FALSE(map_order_status({.found = false}, table).has_value());
}

TEST(HyperliquidMap, StatusVocabularyIsMatchedByItsTail) {
    // The venue adds a spelling for every margin, open-interest and self-trade
    // check, and each is a reason glued in front of a base state. A word added
    // next month must not land in the bucket that refuses to speak about the
    // order at all.
    const auto table = loaded_table();
    const auto classify = [&](std::string_view status) {
        return map_order_status({.order = {.coin = "ETH",
                                           .price = "2461.5",
                                           .qty = "8.99",
                                           .orig_qty = "8.99",
                                           .oid = 1,
                                           .buy = true},
                                 .status = status,
                                 .found = true},
                                table);
    };
    EXPECT_EQ(classify("open")->status, OrderStatus::Accepted);
    EXPECT_EQ(classify("filled")->status, OrderStatus::Filled);
    EXPECT_EQ(classify("canceled")->status, OrderStatus::Canceled);
    EXPECT_EQ(classify("openInterestCapCanceled")->status, OrderStatus::Canceled);
    EXPECT_EQ(classify("scheduledCancel")->status, OrderStatus::Canceled);
    EXPECT_EQ(classify("badAloPxRejected")->status, OrderStatus::Rejected);
    EXPECT_EQ(classify("triggered")->status, OrderStatus::Triggered);
    // An unrecognised word yields nothing: answering Accepted for a status we
    // cannot read would resurrect a dead order.
    EXPECT_FALSE(classify("somethingEntirelyNew").has_value());
}

TEST(HyperliquidWire, FillsCarrySignedFeesAndALiquidityFlag) {
    auto storage = std::make_unique<FillBuffer>();
    const auto body = fixture("rest_user_fills.json");
    const auto fills = parse_fills(body, *storage);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->size(), 5U);
    EXPECT_EQ((*fills)[0].coin, "ETH");
    EXPECT_FALSE((*fills)[0].crossed) << "we made the price";
    EXPECT_EQ((*fills)[0].fee, "-0.000125") << "negative: the venue paid us";
    EXPECT_TRUE((*fills)[1].crossed);
    EXPECT_TRUE((*fills)[1].cloid.empty());
    EXPECT_EQ((*fills)[2].coin, "PURR/USDC");
    // A builder-deployed perp DEX prefixes its coin, and this gateway loaded
    // no such dex.
    EXPECT_EQ((*fills)[3].coin, "xyz:SPCX");
}

TEST(HyperliquidMap, FillsKeepTheRebateSignAndTheLiquiditySide) {
    const auto table = loaded_both();
    auto storage = std::make_unique<FillBuffer>();
    const auto body = fixture("rest_user_fills.json");
    const auto fills = parse_fills(body, *storage);
    ASSERT_TRUE(fills.has_value());

    std::vector<exec::FillReport> mapped;
    for (const auto& wire : *fills) {
        if (auto report = map_fill(wire, table); report.has_value()) {
            mapped.push_back(*report);
        }
    }
    // ETH and BTC map. PURR/USDC is a "Spot Dust Conversion" whose size does
    // not divide by the pair's whole-coin lot, and the two xyz: rows belong to
    // a dex this gateway never loaded.
    ASSERT_EQ(mapped.size(), 2U);

    EXPECT_EQ(mapped[0].instrument.market(), MarketType::Perp);
    EXPECT_EQ(mapped[0].side, Side::Buy);
    EXPECT_EQ(mapped[0].liquidity, Liquidity::Maker);
    EXPECT_LT(mapped[0].fee.raw, 0) << "a rebate must not arrive as a cost";
    EXPECT_EQ(mapped[0].fee.raw, -125'000);
    EXPECT_EQ(mapped[0].exec_id.view(), "892346606631847");
    EXPECT_EQ(mapped[0].exchange_order_id.view(), "527276590284");
    EXPECT_EQ(mapped[0].price.raw, 245'980); // 2459.8 / 0.01
    EXPECT_EQ(mapped[0].qty.raw, 51);        // 0.0051 / 1e-4

    EXPECT_EQ(mapped[1].liquidity, Liquidity::Taker);
    EXPECT_GT(mapped[1].fee.raw, 0);
}

TEST(HyperliquidMap, AUlidAndItsCloidAreTheSameNumber) {
    // A ULID is 48 bits of millisecond and 80 of randomness — 128 exactly, so
    // this is a change of base rather than a hash or a truncation.
    UlidGen gen{11};
    std::array<char, k_cloid_chars> buf{};
    for (int i = 0; i < 64; ++i) {
        const auto id = gen.next(UnixNanos{1787746850812ULL * 1'000'000ULL});
        const auto cloid = cloid_text(id, buf);
        ASSERT_EQ(cloid.size(), k_cloid_chars) << i;
        EXPECT_TRUE(cloid.starts_with("0x"));
        ExchangeClientOrderId back{};
        ASSERT_TRUE(cloid_to_ulid(cloid, back)) << i;
        EXPECT_EQ(back.view(), id.view()) << i;
    }

    // The cloid the venue's own published order vector carries.
    const auto one = *ExchangeClientOrderId::from("00000000000000000000000001");
    EXPECT_EQ(cloid_text(one, buf), "0x00000000000000000000000000000001");

    // Anything that is not exactly "0x" + 32 hex is somebody else's identifier.
    ExchangeClientOrderId ignored{};
    EXPECT_FALSE(cloid_to_ulid("", ignored));
    EXPECT_FALSE(cloid_to_ulid("0x00", ignored));
    EXPECT_FALSE(cloid_to_ulid(std::string(34, 'z'), ignored));
    // And a ULID whose leading digit sets the two bits above 128 is not one.
    EXPECT_TRUE(
        cloid_text(*ExchangeClientOrderId::from("8000000000000000000000000A"), buf).empty());
}

// ----------------------------------------------------------- exchange ----

TEST(HyperliquidExchange, ARealOrderReproducesThePublishedSignature) {
    // The strongest evidence available without a live account: this builds a
    // request the way the gateway does — instrument table, ticks and lots, our
    // msgpack, our EIP-712, our RFC 6979 — and the whole body must equal what
    // the venue's own SDK produces for the same order. Any single wrong byte
    // anywhere in that chain changes the signature.
    const auto table = loaded_table();
    const auto* eth = table.find("ETH");
    ASSERT_NE(eth, nullptr);
    const auto& base = base_of(*eth);

    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = base.id;
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("100"));
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("100"));

    // Asset 1 is ETH's position in meta.universe, which is what the vector was
    // signed with.
    const auto request = builder.create_order(cmd, table, 1, 0);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/exchange");
    EXPECT_EQ(
        request->body,
        R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,)"
        R"("t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"},"nonce":0,"signature":)"
        R"({"r":"0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d2806b100d78e",)"
        R"("s":"0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a3162b6ea940e",)"
        R"("v":28},"vaultAddress":null})");

    // The same order with our client id attached. The cloid is a 34-byte
    // string, which msgpack encodes as str8 — a library defaulting to str16
    // would produce a valid encoding of the same value and an invalid
    // signature, and this is the vector that would catch it.
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("00000000000000000000000001");
    const auto with_cloid = builder.create_order(cmd, table, 1, 0);
    ASSERT_TRUE(with_cloid.has_value());
    EXPECT_EQ(
        with_cloid->body,
        R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,)"
        R"("t":{"limit":{"tif":"Gtc"}},"c":"0x00000000000000000000000000000001"}],)"
        R"("grouping":"na"},"nonce":0,"signature":)"
        R"({"r":"0x41ae18e8239a56cacbc5dad94d45d0b747e5da11ad564077fcac71277a946e3",)"
        R"("s":"0x3c61f667e747404fe7eea8f90ab0e76cc12ce60270438b2058324681a00116da",)"
        R"("v":27},"vaultAddress":null})");

    // Testnet signs a different `source`, so the same order is a different
    // signature. A build that ignored it would place testnet orders on
    // production.
    ExchangeBuilder testnet{wallet, false};
    const auto other = testnet.create_order(cmd, table, 1, 0);
    ASSERT_TRUE(other.has_value());
    EXPECT_NE(other->body, with_cloid->body);
}

TEST(HyperliquidExchange, SpotAndPerpBothBuildAndNameTheirOwnAsset) {
    const auto table = loaded_both();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};

    const auto* hype = table.find("@107");
    ASSERT_NE(hype, nullptr);
    const auto& base = base_of(*hype);
    ASSERT_EQ(base.id.market(), MarketType::Spot);

    OrderCmd cmd{};
    cmd.side = Side::Sell;
    cmd.instrument = base.id;
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("34.624"));
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("30.37"));

    const auto request = builder.create_order(cmd, table, spot_asset_id(107), 0);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    // The spot asset id, not the pair index: asset 107 is an unrelated perp.
    EXPECT_NE(request->body.find(R"("a":10107)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("p":"34.624")"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("s":"30.37")"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("b":false)"), std::string_view::npos);
}

TEST(HyperliquidExchange, PostOnlyReduceOnlyAndIocReachTheWire) {
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};
    const auto& base = base_of(*table.find("ETH"));

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.instrument = base.id;
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("100"));
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("100"));

    // Post-only is a TIME IN FORCE here, not a flag beside one.
    cmd.flags = OrderFlags::PostOnly;
    auto request = builder.create_order(cmd, table, 1, 1);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("tif":"Alo")"), std::string_view::npos);

    cmd.flags = OrderFlags::ReduceOnly;
    cmd.tif = TimeInForce::Ioc;
    request = builder.create_order(cmd, table, 1, 2);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("tif":"Ioc")"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("r":true)"), std::string_view::npos);

    // "Post-only IOC" has no spelling, and dropping either half changes what
    // was asked for.
    cmd.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    EXPECT_EQ(builder.create_order(cmd, table, 1, 3).error(), BuildError::PostOnlyConflictsWithTif);

    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Fok;
    EXPECT_EQ(builder.create_order(cmd, table, 1, 4).error(), BuildError::FokUnsupported);
}

TEST(HyperliquidExchange, AMarketOrderNeedsAPriceBecauseTheVenueHasNone) {
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};
    const auto& base = base_of(*table.find("ETH"));

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Market;
    cmd.instrument = base.id;
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("100"));
    // The reference SDK fakes a market order with an IOC limit at a price it
    // computes from the mid. Inventing that mid here would be a guess made on
    // the caller's behalf with the caller's money.
    EXPECT_EQ(builder.create_order(cmd, table, 1, 1).error(), BuildError::MarketOrderNeedsLimit);

    cmd.price = *base.price_to_ticks(*parse_decimal_e9("100"));
    const auto request = builder.create_order(cmd, table, 1, 2);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("tif":"Ioc")"), std::string_view::npos);
}

TEST(HyperliquidExchange, FiveSignificantFiguresIsCheckedBeforeTheVenueDoes) {
    // Documented and independent of the tick grid: an asset stepping by 0.01
    // still refuses 1234.56. Caught locally so the refusal names the rule.
    EXPECT_TRUE(price_digits_ok("1234.5"));
    EXPECT_FALSE(price_digits_ok("1234.56"));
    EXPECT_TRUE(price_digits_ok("0.001234"));
    EXPECT_TRUE(price_digits_ok("0.0012345"));
    EXPECT_FALSE(price_digits_ok("0.00123456"));
    // Integer prices are always accepted, however many digits they carry.
    EXPECT_TRUE(price_digits_ok("123456"));
    EXPECT_TRUE(price_digits_ok("100"));

    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};
    const auto& base = base_of(*table.find("ETH"));

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.instrument = base.id;
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("1"));
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("1234.56"));
    EXPECT_EQ(builder.create_order(cmd, table, 1, 1).error(), BuildError::TooManySignificantDigits);
}

TEST(HyperliquidExchange, CancelPicksTheActionThatMatchesTheIdItWasGiven) {
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};

    OrderCmd cmd{};
    cmd.kind = CmdKind::Cancel;
    cmd.instrument = base_of(*table.find("ETH")).id;
    // Neither id: there is nothing to name, and guessing would cancel an
    // arbitrary order.
    EXPECT_EQ(builder.cancel_order(cmd, 1, 1).error(), BuildError::NoOrderTarget);

    cmd.target_exchange_order_id = *ExchangeOrderId::from("527278286739");
    const auto by_oid = builder.cancel_order(cmd, 1, 2);
    ASSERT_TRUE(by_oid.has_value());
    EXPECT_NE(by_oid->body.find(R"("type":"cancel")"), std::string_view::npos);
    EXPECT_NE(by_oid->body.find(R"({"a":1,"o":527278286739})"), std::string_view::npos);

    // By client id it is a DIFFERENT action with different key names, not the
    // same one with an alternative field.
    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("00000000000000000000000001");
    const auto by_cloid = builder.cancel_order(cmd, 1, 3);
    ASSERT_TRUE(by_cloid.has_value());
    EXPECT_NE(by_cloid->body.find(R"("type":"cancelByCloid")"), std::string_view::npos);
    EXPECT_NE(by_cloid->body.find(R"({"asset":1,"cloid":"0x00000000000000000000000000000001"})"),
              std::string_view::npos);
}

TEST(HyperliquidExchange, ModifyCarriesTheWholeReplacementOrder) {
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};
    const auto& base = base_of(*table.find("ETH"));

    OrderCmd cmd{};
    cmd.kind = CmdKind::Amend;
    cmd.side = Side::Buy;
    cmd.instrument = base.id;
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("101"));
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("100"));
    cmd.target_exchange_order_id = *ExchangeOrderId::from("527278286739");

    const auto request = builder.modify_order(cmd, table, 1, 1);
    ASSERT_TRUE(request.has_value());
    // A venue id is a NUMBER in `oid` and a client id is a STRING in the same
    // field; msgpack types differ, so this is not a formatting choice.
    EXPECT_NE(request->body.find(R"("oid":527278286739)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("order":{"a":1,)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("p":"101")"), std::string_view::npos);
}

TEST(HyperliquidExchange, UnservedMarketsRefuseBeforeTheInstrumentTable) {
    // InstrumentTable::find compares the WHOLE id, so an id for a market this
    // adapter never loaded resolves to nothing and would otherwise be reported
    // as UnknownInstrument — blaming a stale catalogue for a market that was
    // never served.
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.price = PriceTicks::from_raw(1);
    cmd.qty = QtyLots::from_raw(1);
    for (const auto& [market, expected] :
         {std::pair{MarketType::Option, BuildError::OptionsUnsupported},
          std::pair{MarketType::Future, BuildError::DatedFuturesUnsupported}}) {
        cmd.instrument = InstrumentId::make(VenueId::Hyperliquid, market, SymbolId{0});
        EXPECT_EQ(builder.create_order(cmd, table, 0, 1).error(), expected);
        EXPECT_EQ(builder.cancel_order(cmd, 0, 1).error(), expected);
        EXPECT_EQ(builder.modify_order(cmd, table, 0, 1).error(), expected);
    }
}

TEST(HyperliquidExchange, NothingIsBuiltWithoutAKeyOrAnAssetId) {
    const auto table = loaded_table();
    const Wallet unloaded;
    ExchangeBuilder unsigned_builder{unloaded, true};
    const auto& base = base_of(*table.find("ETH"));

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.instrument = base.id;
    cmd.price = *base.price_to_ticks(*parse_decimal_e9("100"));
    cmd.qty = *base.qty_to_lots(*parse_decimal_e9("100"));
    EXPECT_EQ(unsigned_builder.create_order(cmd, table, 1, 1).error(), BuildError::NotSigned);

    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};
    // An instrument loaded without its venue asset index is not orderable, and
    // asset 0 is a real market — defaulting to it would trade BTC.
    EXPECT_EQ(builder.create_order(cmd, table, -1, 1).error(), BuildError::UnknownAsset);
}

TEST(HyperliquidExchange, NoncesStrictlyIncreaseWhateverTheClockDoes) {
    // The venue keeps the hundred highest nonces per signer and refuses
    // anything at or below the smallest of them, so a repeated millisecond —
    // or a clock stepping backwards — would otherwise cost an order.
    NonceSource nonces;
    const auto now = UnixNanos{1787746850812ULL * 1'000'000ULL};
    const auto first = nonces.next(now);
    EXPECT_EQ(first, 1787746850812ULL);
    const auto second = nonces.next(now);
    EXPECT_GT(second, first);
    const auto backwards = nonces.next(UnixNanos{1'000'000ULL});
    EXPECT_GT(backwards, second);
}

TEST(HyperliquidWire, ARefusedOrderIsStatusOkWithAnErrorInside) {
    auto storage = std::make_unique<ExchangeStatusBuffer>();

    const auto resting_body = fixture("exchange_order_resting.json");
    const auto resting = parse_exchange_reply(resting_body, *storage);
    ASSERT_TRUE(resting.has_value());
    EXPECT_TRUE(resting->ok);
    ASSERT_EQ(resting->statuses.size(), 1U);
    EXPECT_EQ(resting->statuses[0].outcome, ExchangeOutcome::Resting);
    EXPECT_EQ(resting->statuses[0].oid, 77738308ULL);

    const auto filled_body = fixture("exchange_order_filled.json");
    const auto filled = parse_exchange_reply(filled_body, *storage);
    ASSERT_TRUE(filled.has_value());
    ASSERT_EQ(filled->statuses.size(), 1U);
    EXPECT_EQ(filled->statuses[0].outcome, ExchangeOutcome::Filled);
    EXPECT_EQ(filled->statuses[0].total_qty, "0.02");
    EXPECT_EQ(filled->statuses[0].avg_price, "1891.4");

    // The trap: a REFUSED order is `status: "ok"`. Reading the outer status
    // alone reports every rejection as an acceptance.
    const auto error_body = fixture("exchange_order_error.json");
    const auto refused = parse_exchange_reply(error_body, *storage);
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->ok);
    ASSERT_EQ(refused->statuses.size(), 1U);
    EXPECT_EQ(refused->statuses[0].outcome, ExchangeOutcome::Error);
    EXPECT_EQ(refused->statuses[0].error, "Insufficient margin to place order. asset=84");

    // A cancel answers with a bare STRING rather than an object.
    const auto cancel_body = fixture("exchange_cancel_success.json");
    const auto cancelled = parse_exchange_reply(cancel_body, *storage);
    ASSERT_TRUE(cancelled.has_value());
    ASSERT_EQ(cancelled->statuses.size(), 1U);
    EXPECT_EQ(cancelled->statuses[0].outcome, ExchangeOutcome::Success);

    // And a top-level refusal puts PROSE where the object was — the same key,
    // another type — with HTTP 200 beside it.
    const auto err_body = fixture("exchange_err_bad_signature.json");
    const auto refused_request = parse_exchange_reply(err_body, *storage);
    ASSERT_TRUE(refused_request.has_value());
    EXPECT_FALSE(refused_request->ok);
    EXPECT_EQ(refused_request->message, "Unable to recover signer.");
    EXPECT_TRUE(refused_request->statuses.empty());
}

TEST(HyperliquidErrors, RefusalProseIsClassifiedByItsPrefix) {
    // The venue appends context to the refusals that matter most, so equality
    // misses them: "Insufficient margin to place order. asset=84".
    EXPECT_EQ(classify_reason("Insufficient margin to place order. asset=84"),
              exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Price must be divisible by tick size."), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Post only order would have immediately matched, bbo was 100"),
              exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Order must have minimum value of $10."), exec::ErrorClass::Terminal);

    // A broken signature and a well-formed one from an unknown key: both mean
    // nothing this process signs will ever be taken.
    EXPECT_EQ(classify_reason("Unable to recover signer."), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_reason("User or API Wallet 0xb8a6f8b2 does not exist."),
              exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_reason("Must deposit before performing actions. User: 0x123"),
              exec::ErrorClass::AuthFailure);

    // This venue's rate limit is priced in traded volume, not in time.
    EXPECT_EQ(classify_reason("Too many cumulative requests sent (37986 > 10436) for cumulative "
                              "volume traded $437.92."),
              exec::ErrorClass::RateLimited);

    // Unknown prose is Terminal: an unrecognised refusal retried at full speed
    // is how an address earns a ban.
    EXPECT_EQ(classify_reason("something the venue added last week"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason(""), exec::ErrorClass::Terminal);

    // A cancel that arrived after the order was gone did not fail.
    EXPECT_TRUE(is_benign_cancel_failure("Order was never placed, already canceled, or filled."));
    EXPECT_FALSE(is_benign_cancel_failure("Insufficient margin to place order."));
}

} // namespace
} // namespace raikiri::venues::hyperliquid
