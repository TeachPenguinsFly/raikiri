#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Hyperliquid, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Hyperliquid);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(HyperliquidVenue, OrderEntryRefusesPermanently) {
    // Read-only venue: an order must fail loudly, never wait for a verdict
    // that no code will ever produce.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Hyperliquid, .enable_spot = false, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    // A pull this adapter cannot perform must SAY so. Answering with an empty
    // vector would be indistinguishable from "the venue holds nothing of ours",
    // and a reconciler reading it that way concludes we are flat when we are not.
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(exec::caps_of(VenueId::Hyperliquid).position_pull);
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

} // namespace
} // namespace raikiri::venues::hyperliquid
