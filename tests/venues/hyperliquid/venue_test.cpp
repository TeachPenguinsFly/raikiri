#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/hyperliquid/account.hpp>
#include <raikiri/venues/hyperliquid/book.hpp>
#include <raikiri/venues/hyperliquid/catalogue.hpp>
#include <raikiri/venues/hyperliquid/envelope.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/hyperliquid_venue.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/market_data.hpp>
#include <raikiri/venues/hyperliquid/orders.hpp>
#include <raikiri/venues/hyperliquid/positions.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/sign.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::hyperliquid {
namespace {

// l2Book always sends 20 levels a side; nSigFigs only coarsens prices
constexpr std::uint32_t k_hyperliquid_levels = 20;

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// built from the real catalogue so steps derive from the venue's szDecimals
exec::InstrumentTable loaded_table() {
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_meta.json");
    const auto assets = parse_meta(body, *storage);
    EXPECT_TRUE(assets.has_value());
    exec::InstrumentTable table;
    for (const auto& asset : *assets) {
        if (const auto instrument = map_instrument(asset, MarketType::Perp); instrument.has_value()) {
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
    const auto adapter = exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Hyperliquid);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(HyperliquidVenue, AReferenceThatDoesNotResolveReachesTheCallerNamed) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue({.venue = VenueId::Hyperliquid,
                                           .market = MarketType::Perp,
                                           .api_key_env = "env:RAIKIRI_TEST_UNSET_KEY",
                                           .api_secret_env = "nope:secret"},
                                          sink,
                                          clock);
    ASSERT_TRUE(adapter.has_value());
    ASSERT_EQ(adapter->credential_errors.size(), 2U);
    EXPECT_EQ(adapter->credential_errors[0], "api key: secret not found");
    EXPECT_EQ(adapter->credential_errors[1], "api secret: unknown secret scheme");
}

TEST(HyperliquidVenue, OrderEntryWithoutAKeyRefusesThroughTheSink) {
    // without a key the refusal must arrive as an event, since nothing
    // is pushed for an unsent request
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
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

    // no address, so start is refused
    EXPECT_FALSE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());
}

TEST(HyperliquidVenue, ReconciliationPullsRefuseRatherThanReportFlat) {
    // an empty vector would read as flat
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto market : {MarketType::Spot, MarketType::Perp}) {
        const auto fills = adapter->execution->fill_reports(market, UnixNanos{0});
        ASSERT_FALSE(fills.has_value());
        EXPECT_EQ(fills.error().cls, exec::ErrorClass::AuthFailure);
        const auto open = adapter->execution->open_order_reports(market);
        ASSERT_FALSE(open.has_value());
        EXPECT_EQ(open.error().cls, exec::ErrorClass::AuthFailure);
        const auto one = adapter->execution->query_order(*ExchangeClientOrderId::from("00000000000000000000000001"),
                                                         market);
        ASSERT_FALSE(one.has_value());
        EXPECT_EQ(one.error().cls, exec::ErrorClass::AuthFailure);
    }

    const auto perp = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(perp.has_value());
    EXPECT_EQ(perp.error().cls, exec::ErrorClass::AuthFailure);

    // spot has no positions on this venue, so the refusal holds even when fully configured
    const auto spot = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().message.view(), "hyperliquid spot position pull");
}

TEST(HyperliquidVenue, UnservedMarketsRefuseBeforeAnythingElseIsConsulted) {
    // the refusal names the market rather than UnknownInstrument
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Hyperliquid, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto& [market, message] :
         {std::pair{MarketType::Option, std::string_view{"hyperliquid options order entry"}},
          std::pair{MarketType::Future, std::string_view{"hyperliquid dated futures order entry"}}}) {
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

    // the catalogue refuses them without opening a socket
    EXPECT_FALSE(adapter->instruments->fetch_all(MarketType::Option).has_value());
    EXPECT_EQ(adapter->instruments->fetch_all(MarketType::Future).error().message.view(),
              "hyperliquid dated futures catalogue");
}

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
    // allMids is not subscribed, so it resolves to Unknown and is dropped
    const auto frame = fixture("ws_all_mids.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(*event, EventKind::Unknown);
}

TEST(HyperliquidWire, ControlFramesAreNotMistakenForData) {
    const auto bad = fixture("ws_error.json");
    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    EXPECT_FALSE(err->msg.empty());
    // prose, no numeric code
    EXPECT_NE(err->msg.find("Error parsing JSON"), std::string_view::npos);

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
    // ms from a json number, not a digit string
    EXPECT_GT(bbo->ts_ms, 1'700'000'000'000U);
    EXPECT_LT(bbo->ts_ms, 4'000'000'000'000U);

    // views point into the caller's buffer
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
    EXPECT_FALSE(trade.trade_id.empty());
    EXPECT_EQ(trade.trade_id.find('"'), std::string_view::npos);
    // "A" means the aggressor sold; no inversion
    EXPECT_FALSE(trade.aggressor_bought);
}

TEST(HyperliquidWire, OneFrameCanCarrySeveralPrints) {
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trades_batch.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    EXPECT_EQ(trades->size(), 2U);
    EXPECT_EQ((*trades)[0].ts_ms, (*trades)[1].ts_ms);
}

TEST(HyperliquidWire, L2BookIsSnapshotOnlyWithNoContinuityChain) {
    // consecutive frames carry no sequence ids, so the newest frame replaces the book
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
    EXPECT_EQ(next->bids.front().price, first_top_bid);

    // levels is [bids, asks]; bids descend, asks ascend
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
    EXPECT_NE(ctx->mark_price, ctx->oracle_price);
}

TEST(HyperliquidRest, InfoReadsArePostsWithABody) {
    // no get endpoint; the request type lives in the body
    const auto meta = RequestBuilder::meta();
    EXPECT_EQ(meta.method, net::HttpMethod::Post);
    EXPECT_EQ(meta.target, "/info");
    EXPECT_EQ(meta.body, R"({"type":"meta"})");
    // the venue answers 415 without this header
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
    // isDelisted is absent on a live row
    EXPECT_FALSE((*assets)[0].delisted);
    EXPECT_TRUE((*assets)[2].delisted);
    // szDecimals 0 is real; xrp sizes are whole coins
    EXPECT_EQ((*assets)[3].name, "XRP");
    EXPECT_EQ((*assets)[3].sz_decimals, 0);
}

TEST(HyperliquidRest, NullBodyIsRefusedRatherThanReadAsAnEmptyCatalogue) {
    // a `null` body must not read as an empty catalogue
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture("rest_null_body.json");
    EXPECT_FALSE(parse_meta(body, *storage).has_value());
    EXPECT_TRUE(is_null_body(body));
    EXPECT_EQ(classify(200, body), exec::ErrorClass::Terminal);
}

TEST(HyperliquidRest, AssetCtxsPairPositionallyWithTheUniverse) {
    auto assets_storage = std::make_unique<AssetBuffer>();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto assets = parse_meta(body, *assets_storage);
    ASSERT_FALSE(assets.has_value()) << "the combined body's root is an array, not an object";

    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());
    ASSERT_EQ(ctxs->size(), 4U);
    // the rows carry no coin; each takes the name at its position, delisted rows included
    EXPECT_EQ((*ctxs)[0].coin, "BTC");
    EXPECT_EQ((*ctxs)[1].coin, "ETH");
    EXPECT_EQ((*ctxs)[2].coin, "MATIC");
    EXPECT_EQ((*ctxs)[3].coin, "XRP");
    for (const auto& ctx : *ctxs) {
        EXPECT_FALSE(ctx.mark_price.empty());
    }
    // the delisted row's null midPx reads as absent
    EXPECT_TRUE((*ctxs)[2].mid_price.empty());
    EXPECT_FALSE((*ctxs)[0].mid_price.empty());

    // the same parser reads the websocket object
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto pushed = parse_asset_ctx(frame);
    ASSERT_TRUE(pushed.has_value());
    EXPECT_FALSE(pushed->coin.empty());
}

TEST(HyperliquidMap, StepsAreDerivedFromDecimalsAsPowersOfTen) {
    // tick and lot derive from szDecimals in integer fixed point
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

    // btc: szDecimals 5 gives lot 1e-5 and tick 0.1
    const auto btc = map_instrument((*assets)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(base_of(*btc).lot_size.raw, 10'000);
    EXPECT_EQ(base_of(*btc).tick_size.raw, 100'000'000);
    EXPECT_EQ(base_of(*btc).exchange_symbol.view(), "BTC");
    // the wire name stays the bare ticker; the local name is "BTC/USDC"
    EXPECT_EQ(base_of(*btc).local_symbol.view(), "BTC/USDC");
    // sizes are base units
    EXPECT_EQ(base_of(*btc).contract_value.raw, k_e9);
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*btc));
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 3600U);

    // xrp: szDecimals 0 gives lot 1 and tick 1e-6
    const auto xrp = map_instrument((*assets)[3], MarketType::Perp);
    ASSERT_TRUE(xrp.has_value());
    EXPECT_EQ(base_of(*xrp).lot_size.raw, k_e9);
    EXPECT_EQ(base_of(*xrp).tick_size.raw, 1'000);

    // a delisted row never reaches the table
    EXPECT_FALSE(map_instrument((*assets)[2], MarketType::Perp).has_value());
    // spot uses a different catalogue, so a spot request loads no perps
    EXPECT_FALSE(map_instrument((*assets)[0], MarketType::Spot).has_value());
}

TEST(HyperliquidMap, PublishedPricesAllDivideByTheDerivedTick) {
    // published prices honour the decimal cap; both markPx values exceed five significant figures
    const auto table = loaded_table();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    // named so the views outlive the parse
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());

    const auto btc_funding = map_funding((*ctxs)[0], table, UnixNanos{1});
    ASSERT_TRUE(btc_funding.has_value());
    EXPECT_FALSE(btc_funding->mark_price.is_undef());

    const auto xrp_funding = map_funding((*ctxs)[3], table, UnixNanos{1});
    ASSERT_TRUE(xrp_funding.has_value());
    // 1.0195 over a tick of 1e-6
    EXPECT_EQ(xrp_funding->mark_price.raw, 1'019'500);
}

TEST(HyperliquidMap, PriceOffTheDerivedGridIsDroppedNotRounded) {
    // an off-grid value is refused, not rounded
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
    // ts_event is the venue ms, ts_init our ingest ns
    EXPECT_EQ(bbo->ts_event.ns, wire->ts_ms * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 7U);
}

TEST(HyperliquidMap, UnknownCoinMapsToNothing) {
    // a coin the catalogue never loaded has no steps
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
    // a null side yields empty views, not a zero-priced bid
    const auto table = loaded_table();
    WireBbo wire{};
    wire.coin = "BTC";
    wire.ask_price = "64144.0";
    wire.ask_qty = "1.0";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(HyperliquidMap, FundingIsHourlyAndSettlesOnTheNextWholeHour) {
    const auto table = loaded_table();
    const auto frame = fixture("ws_active_asset_ctx.json");
    const auto wire = parse_asset_ctx(frame);
    ASSERT_TRUE(wire.has_value());

    const auto funding = map_funding(*wire, table, UnixNanos{11});
    ASSERT_TRUE(funding.has_value());
    // hourly rate; a cex quotes per 8h
    EXPECT_EQ(funding->rate.raw, 12'500);
    EXPECT_EQ(funding->interval_s, 3600U);
    // mark rides the same payload
    EXPECT_EQ(funding->mark_price.raw, 641'430);
    EXPECT_EQ(funding->index_price.raw, 641'560); // oraclePx 64156.0
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    // settles every whole hour
    EXPECT_EQ(funding->next_funding_time.ns, 3'600'000'000'000ULL);
    const auto later = map_funding(*wire, table, UnixNanos{1'790'000'000'000'000'000ULL});
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(later->next_funding_time.ns, 1'790'002'800'000'000'000ULL);
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    // no timestamp on the payload, so ts_event stays zero
    EXPECT_EQ(funding->ts_event.ns, 0U);
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
    // truncated at 1e-9 rather than refused; the venue writes ten decimals
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'786'723'009'146'250);
    // the ratio of the two volumes is a price, which shows Ntl is quote and Base is coin
    EXPECT_EQ(stats->volume_24h_quote.raw * (k_e9 / k_e6) / stats->volume_24h_base.raw, 64'614);
    EXPECT_EQ(stats->open_interest, *parse_decimal_e9("39664.49642"));
    // no timestamp on either transport, so ts_event is zero
    EXPECT_EQ(stats->ts_event.ns, 0U);
    EXPECT_EQ(stats->ts_init.ns, 11U);
}

TEST(HyperliquidMap, ARestContextPublishesTheSameVolumeAsThePushedOne) {
    const auto table = loaded_table();
    auto ctx_storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs.json");
    const auto ctxs = parse_asset_ctxs(body, *ctx_storage);
    ASSERT_TRUE(ctxs.has_value());
    ASSERT_FALSE(ctxs->empty());
    EXPECT_EQ((*ctxs)[0].day_base_volume, "27654.4941000001");
    const auto stats = map_ticker_stats((*ctxs)[0], table, UnixNanos{1});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote.raw * (k_e9 / k_e6) / stats->volume_24h_base.raw, 64'614);
}

TEST(HyperliquidMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = loaded_table();
    WireAssetCtx wire{};
    wire.coin = "BTC";
    wire.mark_price = "64143.0";
    EXPECT_FALSE(map_funding(wire, table, UnixNanos{1}).has_value());
}

TEST(HyperliquidWs, SubscribeFrameCarriesExactlyOneSubscription) {
    Protocol protocol;
    const auto frame = protocol.subscribe_frame({.channel = "l2Book", .coin = "BTC"});
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}})");

    const auto stop = protocol.unsubscribe_frame({.channel = "l2Book", .coin = "BTC"});
    ASSERT_TRUE(stop.has_value());
    EXPECT_NE(stop->find("unsubscribe"), std::string_view::npos);

    // json both ways
    EXPECT_EQ(Protocol::ping_frame(), R"({"method":"ping"})");
}

TEST(HyperliquidWs, DeclinedKindNeverReachesTheWire) {
    // an empty channel would make the venue close the socket
    Protocol protocol;
    EXPECT_FALSE(protocol.subscribe_frame({.channel = "", .coin = "BTC"}).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({.channel = "bbo", .coin = ""}).has_value());
}

TEST(HyperliquidWs, FundingAndMarkShareOneChannel) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "bbo");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "trades");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "l2Book");
    // one activeAssetCtx subscription carries mark and funding
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "activeAssetCtx");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), channel_for(exec::SubKind::Funding));
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
    // a perp polls its context instead; spot still streams it
    EXPECT_TRUE(stream_channel(exec::SubKind::Funding, MarketType::Perp).empty());
    EXPECT_TRUE(stream_channel(exec::SubKind::MarkPrice, MarketType::Perp).empty());
    EXPECT_EQ(stream_channel(exec::SubKind::Funding, MarketType::Spot), "activeAssetCtx");
    EXPECT_EQ(stream_channel(exec::SubKind::Trades, MarketType::Perp), "trades");
    EXPECT_EQ(stream_path(), "/ws");
    EXPECT_NE(stream_host(true), stream_host(false));
}

TEST(HyperliquidErrors, StatusClassificationDefaultsToTerminal) {
    EXPECT_EQ(classify(400, "Failed to parse the request body as JSON"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(405, ""), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(415, "Expected request with `Content-Type: application/json`"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(422, "Failed to deserialize the JSON body into the target type"), exec::ErrorClass::Terminal);
    // unrecognised statuses are terminal
    EXPECT_EQ(classify(451, ""), exec::ErrorClass::Terminal);

    // a 2xx `null` body is terminal; anything else unparsable is retryable
    EXPECT_EQ(classify(200, "null"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(200, R"({"universe":)"), exec::ErrorClass::Retryable);

    EXPECT_EQ(classify(429, ""), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(500, "null"), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(503, ""), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(403, ""), exec::ErrorClass::AuthFailure);
    // status 0 is a transport failure
    EXPECT_EQ(classify(0, ""), exec::ErrorClass::Connectivity);
}

TEST(HyperliquidErrors, NullBodyDetectionToleratesWhitespace) {
    EXPECT_TRUE(is_null_body("null"));
    EXPECT_TRUE(is_null_body("  null\n"));
    EXPECT_FALSE(is_null_body(""));
    EXPECT_FALSE(is_null_body(R"({"universe":[]})"));
}

TEST(HyperliquidMap, BookSnapshotCrossesIntoTheDomain) {
    // full snapshots, so the mapper keeps no book state
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

    // bids descend, asks ascend, and the sides never cross
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

// every stage is pinned to vectors from hyperliquid-python-sdk tests/signing_test.py

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

// the sdk's test key, which produced every vector below
constexpr std::string_view k_test_key = "0x0123456789012345678901234567890123456789012345678901234567890123";

TEST(HyperliquidSign, KeccakIsKeccakAndNotSha3) {
    // canonical ethereum keccak-256 vectors; sha3-256 would fail them
    EXPECT_EQ(hex_of(keccak_of("")), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    EXPECT_EQ(hex_of(keccak_of("abc")), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    EXPECT_EQ(hex_of(keccak_of("testing")), "5f16f4c7f149ac4f9510d9cf8cf384038ad348b3bcdc01915f95de12df9d1b02");
    EXPECT_EQ(hex_of(keccak_of("hello")), "1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8");

    // 136 bytes is the one length that absorbs a whole padding block
    const std::string block(136, 'a');
    EXPECT_NE(hex_of(keccak_of(block)), hex_of(keccak_of(block + "a")));
    EXPECT_EQ(hex_of(keccak_of(block)).size(), 64U);
}

TEST(HyperliquidSign, ActionHashMatchesThePublishedConnectionId) {
    // from test_phantom_agent_creation_matches_production:
    // eth ioc buy, asset 4, nonce 1677777606040, no vault
    const auto action = unhex("83a474797065a56f72646572a66f72646572739186a16104a162c3a170a6313637302e31a173a6302e3031"
                              "3437a172c2a17481a56c696d697481a3746966a3496f63a867726f7570696e67a26e61");
    const auto hash = action_hash(action, 1677777606040ULL, {}, 0);
    EXPECT_EQ(hex_of(hash), "0fcbeda5ae3c4950a548021552a4fea2226858c4453571bf3f24ba017eac2908");
}

TEST(HyperliquidSign, VaultAndExpiryChangeThePreimageNotJustTheFields) {
    const auto action = unhex("82a474797065a564756d6d79a36e756dcf000000174876e800");
    // absent vault writes 0x00; present writes 0x01 plus 20 address bytes
    EXPECT_EQ(hex_of(action_hash(action, 0, {}, 0)),
              "f528daee6a0bd11407b483cfcd9a48c56884180b70ee86f124053e5fc1bf4d57");
    EXPECT_EQ(hex_of(action_hash(action, 0, "0x1719884eb866cb12b2287399b15f7db5e7d775ea", 0)),
              "de9e09a7a3da45cc694096d4bfdcd89bc1b892d05497c5ecc1f56c335945184c");
}

TEST(HyperliquidSign, MainnetAndTestnetDigestsDiffer) {
    // `source` "a" or "b" separates mainnet from testnet in the signed bytes
    Hash action{};
    action.fill(0x11);
    EXPECT_NE(agent_digest(action, true), agent_digest(action, false));
}

TEST(HyperliquidSign, PublishedSignatureVectorsReproduceExactly) {
    // rfc 6979 is deterministic, so these are equalities
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
    constexpr std::string_view
        k_order = "83a474797065a56f72646572a66f72646572739186a16101a162c3a170a3313030a173a3313030a172c2a174"
                  "81a56c696d697481a3746966a3477463a867726f7570696e67a26e61";
    constexpr std::string_view
        k_order_cloid = "83a474797065a56f72646572a66f72646572739187a16101a162c3a170a3313030a173a3313030a172c2a174"
                        "81a56c696d697481a3746966a3477463a163d92230783030303030303030303030303030303030303030303"
                        "030303030303030303031a867726f7570696e67a26e61";
    constexpr std::string_view
        k_trigger = "83a474797065a56f72646572a66f72646572739186a16101a162c3a170a3313030a173a3313030a172c2a174"
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
    // addresses compare case-insensitively since ethereum checksums by case
    EXPECT_TRUE(
        same_address("0x5e9ee1089755c3435139848e47e6635505d5a13a", "0x5E9EE1089755C3435139848E47E6635505D5A13A"));
    EXPECT_FALSE(
        same_address("0x5e9ee1089755c3435139848e47e6635505d5a13a", "0x5e9ee1089755c3435139848e47e6635505d5a13b"));
    EXPECT_FALSE(same_address("", ""));
    EXPECT_FALSE(same_address("0x5e9ee1", "0x5e9ee1089755c3435139848e47e6635505d5a13a"));

    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    EXPECT_TRUE(same_address(wallet.address(), wallet.address()));
    Wallet other;
    ASSERT_TRUE(other.load("0x0123456789012345678901234567890123456789012345678901234567890124"));
    EXPECT_FALSE(same_address(wallet.address(), other.address()));
}

TEST(HyperliquidSign, KeyMaterialIsValidatedRatherThanTrusted) {
    Wallet wallet;
    EXPECT_FALSE(wallet.load(""));
    EXPECT_FALSE(wallet.load("0x00"));
    EXPECT_FALSE(wallet.load(std::string(64, 'z')));
    // zero and the group order are not valid keys
    EXPECT_FALSE(wallet.load(std::string(64, '0')));
    EXPECT_FALSE(wallet.load("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141"));
    EXPECT_FALSE(wallet.loaded());

    ASSERT_TRUE(wallet.load(k_test_key));
    // same key without the "0x" prefix
    Wallet bare;
    ASSERT_TRUE(bare.load(k_test_key.substr(2)));
    EXPECT_EQ(bare.address(), wallet.address());
    EXPECT_EQ(wallet.address().size(), 42U);
    EXPECT_TRUE(wallet.address().starts_with("0x"));
}

// both markets in one table, as /info returns them together
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
    // matic is delisted and keeps its slot, so xrp is asset 3 though only the third loaded
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

TEST(HyperliquidMap, EverySpotContextNamesItsCoinInOneRead) {
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto meta = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(meta, *storage);
    ASSERT_TRUE(pairs.has_value());
    exec::InstrumentTable table;
    for (const auto& pair : *pairs) {
        if (const auto instrument = map_spot_instrument(pair); instrument.has_value()) {
            ASSERT_TRUE(table.add(*instrument).has_value());
        }
    }

    const auto ctxs_storage = std::make_unique<SpotCtxBuffer>();
    const auto body = fixture("rest_spot_meta_and_asset_ctxs.json");
    const auto rows = parse_spot_asset_ctxs(body, *ctxs_storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    EXPECT_EQ((*rows)[0].coin, "PURR/USDC");
    EXPECT_EQ((*rows)[1].coin, "@107");
    EXPECT_EQ((*rows)[1].day_notional_volume, "64681323.6948000193");

    const auto stats = map_ticker_stats((*rows)[1], table, UnixNanos{9});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->instrument.market(), MarketType::Spot);
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("64681323.694800"));
    EXPECT_GT(stats->volume_24h_base.raw, 691'918'000'000'000);
    EXPECT_TRUE(stats->open_interest.is_undef());
}

TEST(HyperliquidMap, SpotPairsLoadFromTheirBaseTokenPrecision) {
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto body = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(body, *storage);
    ASSERT_TRUE(pairs.has_value());
    ASSERT_EQ(pairs->size(), 7U);

    // szDecimals lives on the base token
    EXPECT_EQ((*pairs)[1].name, "@107");
    EXPECT_EQ((*pairs)[1].base, "HYPE");
    EXPECT_EQ((*pairs)[1].quote, "USDC");
    EXPECT_EQ((*pairs)[1].index, 107);
    EXPECT_EQ((*pairs)[1].sz_decimals, 2);

    const auto hype = map_spot_instrument((*pairs)[1]);
    ASSERT_TRUE(hype.has_value());
    const auto& base = base_of(*hype);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    // spot prices carry 8 - szDecimals places
    EXPECT_EQ(base.tick_size.raw, parse_decimal_e9("0.000001")->raw);
    EXPECT_EQ(base.lot_size.raw, parse_decimal_e9("0.01")->raw);
    // the wire coin is the key
    EXPECT_EQ(base.exchange_symbol.view(), "@107");
    EXPECT_EQ(base.local_symbol.view(), "HYPE/USDC");

    // PURR/USDC is spelled by name on the wire
    const auto purr = map_spot_instrument((*pairs)[0]);
    ASSERT_TRUE(purr.has_value());
    EXPECT_EQ(base_of(*purr).exchange_symbol.view(), "PURR/USDC");
    EXPECT_EQ(base_of(*purr).lot_size.raw, parse_decimal_e9("1")->raw);
    EXPECT_EQ(base_of(*purr).tick_size.raw, parse_decimal_e9("0.00000001")->raw);
}

TEST(HyperliquidMap, SpotPairsAreNamedByBothTokensAndNotByTheBaseAlone) {
    // hype lists against four quote tokens, four separate books
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto body = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(body, *storage);
    ASSERT_TRUE(pairs.has_value());

    const auto name_of = [&pairs](std::string_view coin) {
        std::string found;
        for (const auto& pair : *pairs) {
            if (pair.name != coin) {
                continue;
            }
            const auto instrument = map_spot_instrument(pair);
            EXPECT_TRUE(instrument.has_value()) << coin;
            if (instrument.has_value()) {
                found = std::string{base_of(*instrument).local_symbol.view()};
            }
        }
        return found;
    };
    EXPECT_EQ(name_of("@107"), "HYPE/USDC");
    EXPECT_EQ(name_of("@207"), "HYPE/USDT0");
    EXPECT_EQ(name_of("@232"), "HYPE/USDH");
    EXPECT_EQ(name_of("@255"), "HYPE/USDE");
    // axl lists only against usdh
    EXPECT_EQ(name_of("@252"), "AXL/USDH");
    // steps come from the base token
    const auto usdh = map_spot_instrument((*pairs)[3]);
    ASSERT_TRUE(usdh.has_value());
    EXPECT_EQ(base_of(*usdh).exchange_symbol.view(), "@232");
    EXPECT_EQ(base_of(*usdh).lot_size.raw, parse_decimal_e9("0.01")->raw);
    EXPECT_EQ(base_of(*usdh).tick_size.raw, parse_decimal_e9("0.000001")->raw);
}

TEST(HyperliquidMap, NoTwoSpotListingsShareOneCanonicalSymbol) {
    // no canonical symbol appears twice across the catalogue
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto body = fixture("rest_spot_meta.json");
    const auto pairs = parse_spot_meta(body, *storage);
    ASSERT_TRUE(pairs.has_value());

    std::vector<std::string> canonical;
    std::vector<std::string> wire;
    for (const auto& pair : *pairs) {
        const auto instrument = map_spot_instrument(pair);
        ASSERT_TRUE(instrument.has_value()) << pair.name;
        canonical.emplace_back(base_of(*instrument).local_symbol.view());
        wire.emplace_back(base_of(*instrument).exchange_symbol.view());
    }
    ASSERT_EQ(canonical.size(), pairs->size());
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        for (std::size_t j = i + 1; j < canonical.size(); ++j) {
            EXPECT_NE(canonical[i], canonical[j])
                << "wire " << wire[i] << " and " << wire[j] << " collapse into one canonical name";
        }
    }
}

TEST(HyperliquidWire, ASpotPairNamingAnUncarriedTokenIsDropped) {
    // synthetic body; a pair missing either token is dropped
    constexpr std::string_view k_body = R"({"universe":[{"tokens":[1,0],"name":"@1","index":1,"isCanonical":false},)"
                                        R"({"tokens":[1,9],"name":"@2","index":2,"isCanonical":false},)"
                                        R"({"tokens":[9,0],"name":"@3","index":3,"isCanonical":false}],)"
                                        R"("tokens":[{"name":"USDC","szDecimals":8,"index":0},)"
                                        R"({"name":"PURR","szDecimals":0,"index":1}]})";
    auto storage = std::make_unique<SpotAssetBuffer>();
    const auto pairs = parse_spot_meta(k_body, *storage);
    ASSERT_TRUE(pairs.has_value());
    ASSERT_EQ(pairs->size(), 1U);
    EXPECT_EQ((*pairs)[0].name, "@1");

    // a row that is not [base, quote] fails the read
    constexpr std::string_view
        k_three = R"({"universe":[{"tokens":[1,0,1],"name":"@1","index":1,"isCanonical":false}],)"
                  R"("tokens":[{"name":"USDC","szDecimals":8,"index":0},)"
                  R"({"name":"PURR","szDecimals":0,"index":1}]})";
    EXPECT_FALSE(parse_spot_meta(k_three, *storage).has_value());
}

TEST(HyperliquidMap, SpotAssetIdsSitTenThousandAboveThePerpSpace) {
    // asset 107 is a perp and 10107 is HYPE/USDC
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
    // `szi` is signed
    EXPECT_EQ((*positions)[0].size, "-268.52681");
    EXPECT_EQ((*positions)[0].entry_px, "74044.9");
    // one `time` for the whole snapshot
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
    // FARTCOIN is not in this trimmed catalogue, so it maps to nothing
    ASSERT_EQ(reports.size(), 3U);
    EXPECT_LT(reports[0].net_qty.raw, 0) << "a short must not arrive as a long";
    EXPECT_EQ(reports[0].net_qty.raw, -26'852'681); // -268.52681 / 1e-5
    EXPECT_EQ(reports[0].avg_entry.raw, 740'449);   // 74044.9 / 0.1
    EXPECT_EQ(reports[2].net_qty.raw, -3'082'681);  // XRP trades whole coins
    EXPECT_EQ(reports[0].ts_venue.ns, 1787746850812ULL * 1'000'000ULL);
}

TEST(HyperliquidMap, PositionSizesSurviveTheVenuesFloatSerialiser) {
    // `szi` is float-serialised; digits past 1e-9 drop, off-step sizes are still refused
    const auto table = loaded_table();
    const auto* eth = table.find("ETH");
    ASSERT_NE(eth, nullptr);
    const auto ts = UnixNanos{7};

    const auto noisy = map_position({.coin = "ETH", .size = "-24856.6978000000004", .entry_px = "1981.64"}, table, ts);
    ASSERT_TRUE(noisy.has_value());
    EXPECT_EQ(noisy->net_qty.raw, -248'566'978);

    // half a lot is refused; eth steps by 1e-4
    EXPECT_FALSE(map_position({.coin = "ETH", .size = "1.00005", .entry_px = "1981.64"}, table, ts).has_value());
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
    // an order placed without a cloid has no `cloid` key
    EXPECT_EQ((*orders)[0].cloid, "0x00000000000000000000000325566338");
    EXPECT_TRUE((*orders)[1].cloid.empty());
    // the plain form says nothing about post-only or triggers
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
    // a json null cloid reads as absent
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
    // the cloid decodes back to our 26-char ulid
    EXPECT_EQ(perp[0].exchange_client_order_id.view().size(), 26U);

    // HYPE/USDC steps by 1e-6 in price and 0.01 in size.
    EXPECT_EQ(spot[2].price.raw, 34'624'000);
    EXPECT_EQ(spot[2].qty.raw, 3'037);
}

TEST(HyperliquidMap, PartialFillShowsAsLeavesBelowTheOriginal) {
    const auto table = loaded_table();
    const auto report = map_open_order(
        {.coin = "ETH", .price = "2461.5", .qty = "4.0", .orig_qty = "8.99", .oid = 1, .buy = true}, table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->qty.raw, 89'900);
    EXPECT_EQ(report->leaves.raw, 40'000);
}

TEST(HyperliquidMap, ADormantTriggerOrderIsNotReportedAsResting) {
    // a trigger's `limitPx` is where it will rest, so it is not reported
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

    // unknown and forgotten ids answer identically, not as an error
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
    // a terminal order has no leaves
    EXPECT_EQ(report->leaves.raw, 0);
    EXPECT_EQ(report->ts_venue.ns, 1787746854240ULL * 1'000'000ULL);

    EXPECT_FALSE(map_order_status({.found = false}, table).has_value());
}

TEST(HyperliquidMap, StatusVocabularyIsMatchedByItsTail) {
    // status words are base states with reasons prefixed; new ones must still classify
    const auto table = loaded_table();
    const auto classify = [&](std::string_view status) {
        return map_order_status(
            {.order = {.coin = "ETH", .price = "2461.5", .qty = "8.99", .orig_qty = "8.99", .oid = 1, .buy = true},
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
    // an unrecognised word yields nothing
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
    // a hip-3 dex prefixes its coin, and none is loaded here
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
    // PURR/USDC dust conversion is off-lot and the xyz: rows are an unloaded dex
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

TEST(HyperliquidWire, FundingRowsCarryTheMoneyUnderDeltaAndTheStampBesideIt) {
    auto storage = std::make_unique<FundingChargeBuffer>();
    const auto body = fixture("rest_user_funding.json");
    const auto rows = parse_user_funding(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    EXPECT_EQ((*rows)[0].coin, "BTC");
    EXPECT_EQ((*rows)[0].type, "funding");
    EXPECT_EQ((*rows)[0].usdc, "-0.185819");
    EXPECT_EQ((*rows)[0].time_ms, 1'789'239'600'008ULL);
    // two coins settle on one stamp, so the key is coin plus stamp
    EXPECT_EQ((*rows)[1].time_ms, (*rows)[0].time_ms);
}

TEST(HyperliquidMap, AFundingPaymentAndAFundingReceiptDoNotBothComeOutPositive) {
    // `usdc` is the signed wallet delta
    const auto table = loaded_table();
    auto storage = std::make_unique<FundingChargeBuffer>();
    const auto body = fixture("rest_user_funding.json");
    const auto rows = parse_user_funding(body, *storage);
    ASSERT_TRUE(rows.has_value());

    std::vector<LedgerEntry> booked;
    for (const auto& wire : *rows) {
        if (const auto charge = map_funding_charge(wire, table, UnixNanos{7}); charge.has_value()) {
            booked.push_back(*charge);
        }
    }
    // SOL is not loaded, so it is dropped
    ASSERT_EQ(booked.size(), 2U);

    EXPECT_EQ(booked[0].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[0].amount.raw, -185'819'000) << "0.185819 paid away";
    EXPECT_EQ(booked[1].amount.raw, 583'839'000) << "0.583839 received, and NOT flipped";
    EXPECT_EQ(booked[0].settled_at.ns, 1'789'239'600'008ULL * 1'000'000ULL);
    EXPECT_EQ(booked[0].ts_init.ns, 7U);
}

TEST(HyperliquidMap, TheFundingReferenceIsBuiltBecauseTheHashIsAllZeros) {
    // the row hash is all zeroes, so the key is coin plus stamp
    const auto table = loaded_table();
    auto storage = std::make_unique<FundingChargeBuffer>();
    const auto body = fixture("rest_user_funding.json");
    const auto rows = parse_user_funding(body, *storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_funding_charge((*rows)[0], table, UnixNanos{7});
    const auto eth = map_funding_charge((*rows)[1], table, UnixNanos{7});
    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(eth.has_value());
    EXPECT_EQ(btc->reference.view(), "BTC:1789239600008");
    EXPECT_EQ(eth->reference.view(), "ETH:1789239600008");
}

TEST(HyperliquidRest, TheFundingReadCarriesTheCursorItWasAskedFor) {
    // `startTime` is required on this read
    constexpr std::string_view address = "0x1719884eb866cb12b2287399b15f7db5e7d775ea";
    UserRequestBuilder builder;
    const auto request = builder.user_funding(address, 1'789'239'600'008ULL);
    EXPECT_EQ(request.method, net::HttpMethod::Post);
    EXPECT_EQ(request.target, "/info");
    EXPECT_EQ(request.body,
              R"({"type":"userFunding","user":"0x1719884eb866cb12b2287399b15f7db5e7d775ea",)"
              R"("startTime":1789239600008})");

    const auto from_zero = builder.user_funding(address, 0);
    EXPECT_NE(std::string{from_zero.body}.find(R"("startTime":0)"), std::string::npos);
}

TEST(HyperliquidMap, AUlidAndItsCloidAreTheSameNumber) {
    // a ulid is 128 bits, so this is a change of base
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

    // the cloid from the venue's published order vector
    const auto one = *ExchangeClientOrderId::from("00000000000000000000000001");
    EXPECT_EQ(cloid_text(one, buf), "0x00000000000000000000000000000001");

    // anything but "0x" + 32 hex is refused
    ExchangeClientOrderId ignored{};
    EXPECT_FALSE(cloid_to_ulid("", ignored));
    EXPECT_FALSE(cloid_to_ulid("0x00", ignored));
    EXPECT_FALSE(cloid_to_ulid(std::string(34, 'z'), ignored));
    // a ulid whose leading digit sets bits above 128 is refused
    EXPECT_TRUE(cloid_text(*ExchangeClientOrderId::from("8000000000000000000000000A"), buf).empty());
}

TEST(HyperliquidExchange, ARealOrderReproducesThePublishedSignature) {
    // the whole body must equal the sdk's output for the same order
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

    // asset 1 is eth's position in meta.universe
    const auto request = builder.create_order(cmd, table, 1, 0);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/exchange");
    EXPECT_EQ(request->body,
              R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,)"
              R"("t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"},"nonce":0,"signature":)"
              R"({"r":"0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d2806b100d78e",)"
              R"("s":"0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a3162b6ea940e",)"
              R"("v":28},"vaultAddress":null})");

    // with a 34-byte cloid, which msgpack encodes as str8
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("00000000000000000000000001");
    const auto with_cloid = builder.create_order(cmd, table, 1, 0);
    ASSERT_TRUE(with_cloid.has_value());
    EXPECT_EQ(with_cloid->body,
              R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"100","s":"100","r":false,)"
              R"("t":{"limit":{"tif":"Gtc"}},"c":"0x00000000000000000000000000000001"}],)"
              R"("grouping":"na"},"nonce":0,"signature":)"
              R"({"r":"0x41ae18e8239a56cacbc5dad94d45d0b747e5da11ad564077fcac71277a946e3",)"
              R"("s":"0x3c61f667e747404fe7eea8f90ab0e76cc12ce60270438b2058324681a00116da",)"
              R"("v":27},"vaultAddress":null})");

    // testnet signs a different `source`
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
    // the spot asset id, not the pair index
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

    // post-only is a time in force here
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

    // post-only ioc cannot be expressed
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
    // no mid is invented for a market order
    EXPECT_EQ(builder.create_order(cmd, table, 1, 1).error(), BuildError::MarketOrderNeedsLimit);

    cmd.price = *base.price_to_ticks(*parse_decimal_e9("100"));
    const auto request = builder.create_order(cmd, table, 1, 2);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("tif":"Ioc")"), std::string_view::npos);
}

TEST(HyperliquidExchange, FiveSignificantFiguresIsCheckedBeforeTheVenueDoes) {
    // five significant figures regardless of tick
    EXPECT_TRUE(price_digits_ok("1234.5"));
    EXPECT_FALSE(price_digits_ok("1234.56"));
    EXPECT_TRUE(price_digits_ok("0.001234"));
    EXPECT_TRUE(price_digits_ok("0.0012345"));
    EXPECT_FALSE(price_digits_ok("0.00123456"));
    // integer prices are always accepted
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
    // neither id, so nothing to cancel
    EXPECT_EQ(builder.cancel_order(cmd, 1, 1).error(), BuildError::NoOrderTarget);

    cmd.target_exchange_order_id = *ExchangeOrderId::from("527278286739");
    const auto by_oid = builder.cancel_order(cmd, 1, 2);
    ASSERT_TRUE(by_oid.has_value());
    EXPECT_NE(by_oid->body.find(R"("type":"cancel")"), std::string_view::npos);
    EXPECT_NE(by_oid->body.find(R"({"a":1,"o":527278286739})"), std::string_view::npos);

    // by client id it is a separate action with different keys
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
    // an oid is a number and a cloid a string in msgpack
    EXPECT_NE(request->body.find(R"("oid":527278286739)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("order":{"a":1,)"), std::string_view::npos);
    EXPECT_NE(request->body.find(R"("p":"101")"), std::string_view::npos);
}

TEST(HyperliquidExchange, UnservedMarketsRefuseBeforeTheInstrumentTable) {
    // unserved markets are refused before the table lookup
    const auto table = loaded_table();
    Wallet wallet;
    ASSERT_TRUE(wallet.load(k_test_key));
    ExchangeBuilder builder{wallet, true};

    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.price = PriceTicks::from_raw(1);
    cmd.qty = QtyLots::from_raw(1);
    for (const auto& [market, expected] : {std::pair{MarketType::Option, BuildError::OptionsUnsupported},
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
    // asset 0 is a real market, so a missing index is refused
    EXPECT_EQ(builder.create_order(cmd, table, -1, 1).error(), BuildError::UnknownAsset);
}

TEST(HyperliquidExchange, NoncesStrictlyIncreaseWhateverTheClockDoes) {
    // nonces stay strictly increasing even within one ms
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

    // a refused order is `status: "ok"`
    const auto error_body = fixture("exchange_order_error.json");
    const auto refused = parse_exchange_reply(error_body, *storage);
    ASSERT_TRUE(refused.has_value());
    EXPECT_TRUE(refused->ok);
    ASSERT_EQ(refused->statuses.size(), 1U);
    EXPECT_EQ(refused->statuses[0].outcome, ExchangeOutcome::Error);
    EXPECT_EQ(refused->statuses[0].error, "Insufficient margin to place order. asset=84");

    // a cancel answers with a bare string
    const auto cancel_body = fixture("exchange_cancel_success.json");
    const auto cancelled = parse_exchange_reply(cancel_body, *storage);
    ASSERT_TRUE(cancelled.has_value());
    ASSERT_EQ(cancelled->statuses.size(), 1U);
    EXPECT_EQ(cancelled->statuses[0].outcome, ExchangeOutcome::Success);

    // a top-level refusal puts prose in `response` with http 200
    const auto err_body = fixture("exchange_err_bad_signature.json");
    const auto refused_request = parse_exchange_reply(err_body, *storage);
    ASSERT_TRUE(refused_request.has_value());
    EXPECT_FALSE(refused_request->ok);
    EXPECT_EQ(refused_request->message, "Unable to recover signer.");
    EXPECT_TRUE(refused_request->statuses.empty());
}

TEST(HyperliquidErrors, RefusalProseIsClassifiedByItsPrefix) {
    // matched by prefix since the venue appends context
    EXPECT_EQ(classify_reason("Insufficient margin to place order. asset=84"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Price must be divisible by tick size."), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Post only order would have immediately matched, bbo was 100"),
              exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason("Order must have minimum value of $10."), exec::ErrorClass::Terminal);

    // both mean nothing this key signs will be accepted
    EXPECT_EQ(classify_reason("Unable to recover signer."), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_reason("User or API Wallet 0xb8a6f8b2 does not exist."), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_reason("Must deposit before performing actions. User: 0x123"), exec::ErrorClass::AuthFailure);

    // the limit is priced in traded volume
    EXPECT_EQ(classify_reason("Too many cumulative requests sent (37986 > 10436) for cumulative "
                              "volume traded $437.92."),
              exec::ErrorClass::RateLimited);

    // unknown prose is terminal
    EXPECT_EQ(classify_reason("something the venue added last week"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_reason(""), exec::ErrorClass::Terminal);

    // a late cancel is not a failure
    EXPECT_TRUE(is_benign_cancel_failure("Order was never placed, already canceled, or filled."));
    EXPECT_FALSE(is_benign_cancel_failure("Insufficient margin to place order."));
}

exec::InstrumentTable dex_table(std::string_view meta = "rest_meta_dex_xyz.json") {
    auto storage = std::make_unique<AssetBuffer>();
    const auto body = fixture(meta);
    const auto assets = parse_meta(body, *storage);
    EXPECT_TRUE(assets.has_value());
    exec::InstrumentTable table;
    for (const auto& asset : *assets) {
        if (const auto instrument = map_dex_instrument(asset, "USDC"); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

TEST(HyperliquidHip3, PerpDexsSkipsTheMainDexNull) {
    PerpDexBuffer storage;
    const auto body = fixture("rest_perp_dexs.json");
    const auto names = parse_perp_dexs(body, storage);
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names->size(), 10U);
    EXPECT_EQ((*names)[0], "xyz");
    EXPECT_NE(std::ranges::find(*names, "para"), names->end());
    EXPECT_EQ(std::ranges::find(*names, ""), names->end());
}

TEST(HyperliquidHip3, ADexRequestNamesTheDex) {
    std::string body;
    const auto request = RequestBuilder::meta("xyz", body);
    EXPECT_EQ(request.target, "/info");
    EXPECT_EQ(request.body, R"({"type":"meta","dex":"xyz"})");
    EXPECT_EQ(RequestBuilder::perp_dexs().body, R"({"type":"perpDexs"})");
}

TEST(HyperliquidHip3, ADexMetaCarriesItsCollateralToken) {
    auto storage = std::make_unique<AssetBuffer>();
    const auto xyz = fixture("rest_meta_dex_xyz.json");
    const auto rows = parse_meta(xyz, *storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 125U);
    EXPECT_EQ(storage->collateral_token, 0);
    EXPECT_EQ((*rows)[0].name, "xyz:XYZ100");

    const auto flx = fixture("rest_meta_dex_flx.json");
    ASSERT_TRUE(parse_meta(flx, *storage).has_value());
    EXPECT_EQ(storage->collateral_token, 360);
    const auto spot = fixture("rest_spot_meta.json");
    const auto token = parse_token_name(spot, 360);
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "USDH");
    EXPECT_FALSE(parse_token_name(spot, 999'999).has_value());

    const auto main = fixture("rest_meta.json");
    ASSERT_TRUE(parse_meta(main, *storage).has_value());
    EXPECT_EQ(storage->collateral_token, 0) << "the main dex states none and margins in USDC";
}

TEST(HyperliquidHip3, TheDexStaysInBothSymbolsAndTheQuoteIsTheCollateral) {
    const auto table = dex_table();
    EXPECT_EQ(table.size(), 109U) << "125 rows, 16 delisted";
    const auto* tsla = table.find(std::string_view{"xyz:TSLA"});
    ASSERT_NE(tsla, nullptr);
    const auto& base = base_of(*tsla);
    EXPECT_EQ(base.exchange_symbol.view(), "xyz:TSLA");
    EXPECT_EQ(base.local_symbol.view(), "xyz:TSLA/USDC");
    EXPECT_EQ(base.id.venue(), VenueId::Hyperliquid);
    EXPECT_EQ(base.id.market(), MarketType::Perp);
    EXPECT_EQ(base.lot_size.raw, 1'000'000) << "szDecimals 3";
    EXPECT_EQ(base.tick_size.raw, 1'000'000) << "6 - szDecimals, as on the main dex";
    const auto* eur = table.find(std::string_view{"xyz:EUR"});
    ASSERT_NE(eur, nullptr);
    EXPECT_EQ(base_of(*eur).tick_size.raw, 10'000) << "szDecimals 1 quotes five places";

    auto storage = std::make_unique<AssetBuffer>();
    const auto flx = fixture("rest_meta_dex_flx.json");
    const auto rows = parse_meta(flx, *storage);
    ASSERT_TRUE(rows.has_value());
    WireAsset live = (*rows)[0];
    live.delisted = false;
    const auto usdh = map_dex_instrument(live, "USDH");
    ASSERT_TRUE(usdh.has_value());
    EXPECT_EQ(base_of(*usdh).local_symbol.view(), "flx:TSLA/USDH");
    EXPECT_FALSE(map_dex_instrument((*rows)[0], "USDH").has_value()) << "delisted";
}

TEST(HyperliquidHip3, EachMapperRefusesTheOthersRows) {
    const WireAsset dex{.name = "xyz:TSLA", .sz_decimals = 3, .max_leverage = 20};
    const WireAsset main{.name = "BTC", .sz_decimals = 5, .max_leverage = 40};
    EXPECT_FALSE(map_instrument(dex, MarketType::Perp).has_value());
    EXPECT_FALSE(map_dex_instrument(main, "USDC").has_value());
    EXPECT_FALSE(map_dex_instrument({.name = ":TSLA", .sz_decimals = 3}, "USDC").has_value());
    EXPECT_FALSE(map_dex_instrument(dex, "").has_value());
}

TEST(HyperliquidHip3, EveryLiveMarkOnTheDexSitsOnItsDerivedTick) {
    const auto table = dex_table();
    auto storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs_dex_xyz.json");
    const auto ctxs = parse_asset_ctxs(body, *storage);
    ASSERT_TRUE(ctxs.has_value());
    auto meta = std::make_unique<AssetBuffer>();
    const auto meta_body = fixture("rest_meta_dex_xyz.json");
    const auto rows = parse_meta(meta_body, *meta);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(ctxs->size(), rows->size());
    std::size_t checked = 0;
    for (std::size_t i = 0; i < rows->size(); ++i) {
        const auto* instrument = table.find((*rows)[i].name);
        if (instrument == nullptr) {
            continue;
        }
        EXPECT_TRUE(price_from_wire((*ctxs)[i].mark_price, base_of(*instrument)).has_value())
            << (*rows)[i].name << " mark " << (*ctxs)[i].mark_price;
        ++checked;
    }
    EXPECT_EQ(checked, 109U);
}

TEST(HyperliquidHip3, EveryChannelNamesTheCoinWithItsDex) {
    const auto table = dex_table();
    const auto ts = UnixNanos{1'790'412'300'000'000'000ULL};
    const auto* coin = table.find(std::string_view{"xyz:XYZ100"});
    ASSERT_NE(coin, nullptr);
    const auto id = base_of(*coin).id;

    const auto bbo_frame = fixture("ws_hip3_bbo.json");
    EXPECT_EQ(parse_event(bbo_frame), EventKind::Bbo);
    const auto bbo = parse_bbo(bbo_frame);
    ASSERT_TRUE(bbo.has_value());
    const auto quote = map_bbo(*bbo, table, ts);
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->instrument, id);
    EXPECT_EQ(quote->bid.raw, 3'056'700) << "szDecimals 4, so a cent tick";
    EXPECT_EQ(quote->ask.raw, 3'056'800);

    TradeBuffer trades;
    const auto trade_frame = fixture("ws_hip3_trades.json");
    const auto prints = parse_trades(trade_frame, trades);
    ASSERT_TRUE(prints.has_value());
    ASSERT_FALSE(prints->empty());
    const auto trade = map_trade((*prints)[0], table, ts);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->instrument, id);
    EXPECT_EQ(trade->price.raw, 3'056'700);

    LevelBuffer levels;
    const auto book_frame = fixture("ws_hip3_l2book.json");
    const auto book = parse_book(book_frame, levels);
    ASSERT_TRUE(book.has_value());
    const auto snapshot = map_book(*book, table, ts);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->instrument, id);
    EXPECT_GT(snapshot->bid_count, 0U);

    const auto ctx_frame = fixture("ws_hip3_active_asset_ctx.json");
    const auto ctx = parse_asset_ctx(ctx_frame);
    ASSERT_TRUE(ctx.has_value());
    const auto funding = map_funding(*ctx, table, ts);
    ASSERT_TRUE(funding.has_value()) << "ten fractional digits: " << ctx->funding;
    EXPECT_EQ(funding->instrument, id);
    EXPECT_EQ(funding->rate.raw, 5'564) << "0.0000055648, below 1e-9 dropped";
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->next_funding_time.ns % 3'600'000'000'000ULL, 0U) << "a hip-3 dex settles hourly too";
    EXPECT_GT(funding->next_funding_time.ns, ts.ns);
    EXPECT_EQ(funding->mark_price.raw, 3'056'900);
    const auto stats = map_ticker_stats(*ctx, table, ts);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote.raw, 266'855'056'880'799LL);
}

InstrumentBase classified_dex(std::string_view name, std::span<const WireCategory> categories) {
    auto instrument = map_dex_instrument({.name = name, .sz_decimals = 3, .max_leverage = 10}, "USDC");
    EXPECT_TRUE(instrument.has_value()) << name;
    if (!instrument.has_value()) {
        return InstrumentBase{};
    }
    std::visit([&](auto& row) { row.base.asset_class = dex_asset_class(name, categories); }, *instrument);
    classify(VenueId::Hyperliquid, *instrument);
    return base_of(*instrument);
}

TEST(HyperliquidHip3, ADexRowTakesTheVenuesCategoryAndNeverDefaults) {
    const auto body = fixture("rest_perp_categories.json");
    auto storage = std::make_unique<PerpCategoryBuffer>();
    const auto categories = parse_perp_categories(body, *storage);
    ASSERT_TRUE(categories.has_value());
    EXPECT_EQ(categories->size(), 206U);
    EXPECT_EQ(RequestBuilder::perp_categories().body, R"({"type":"perpCategories"})");

    const auto tsla = classified_dex("xyz:TSLA", *categories);
    EXPECT_EQ(tsla.local_symbol.view(), "xyz:TSLA/USDC");
    EXPECT_EQ(tsla.asset_class, AssetClass::Equity);
    EXPECT_EQ(tsla.underlying.view(), "TSLA");

    const auto gold = classified_dex("xyz:GOLD", *categories);
    EXPECT_EQ(gold.asset_class, AssetClass::Commodity);
    EXPECT_EQ(gold.underlying.view(), "XAU");

    const auto sp500 = classified_dex("xyz:SP500", *categories);
    EXPECT_EQ(sp500.asset_class, AssetClass::Index);
    EXPECT_EQ(sp500.underlying.view(), "SPX");

    const auto yen = classified_dex("xyz:JPY", *categories);
    EXPECT_EQ(yen.asset_class, AssetClass::FX);
    EXPECT_EQ(yen.underlying.view(), "USDJPY");

    const auto samsung = classified_dex("xyz:SMSN", *categories);
    EXPECT_EQ(samsung.asset_class, AssetClass::Equity);
    EXPECT_EQ(samsung.underlying.view(), "SAMSUNG");

    const auto btc = classified_dex("flx:BTC", *categories);
    EXPECT_EQ(btc.asset_class, AssetClass::Crypto);
    EXPECT_EQ(btc.underlying.view(), "BTC");

    const auto ewy = classified_dex("io:EWY", *categories);
    EXPECT_EQ(ewy.asset_class, AssetClass::Equity);
    EXPECT_EQ(ewy.underlying.view(), "EWY");

    const auto unlisted = classified_dex("xyz:NOTLISTED", *categories);
    EXPECT_EQ(unlisted.asset_class, AssetClass::Unknown);
    EXPECT_TRUE(unlisted.underlying.empty());

    auto coin = map_instrument({.name = "SPX", .sz_decimals = 1, .max_leverage = 5}, MarketType::Perp);
    ASSERT_TRUE(coin.has_value());
    classify(VenueId::Hyperliquid, *coin);
    EXPECT_EQ(base_of(*coin).asset_class, AssetClass::Crypto);
    EXPECT_EQ(base_of(*coin).underlying.view(), "SPX");
}

TEST(HyperliquidHip3, EveryLiveDexRowHasAClass) {
    const auto body = fixture("rest_perp_categories.json");
    auto storage = std::make_unique<PerpCategoryBuffer>();
    const auto categories = parse_perp_categories(body, *storage);
    ASSERT_TRUE(categories.has_value());
    std::size_t live = 0;
    for (const auto* dex : {"xyz", "para", "mkts", "io"}) {
        const auto meta = fixture(std::string{"rest_meta_dex_"} + dex + "_live.json");
        auto assets_storage = std::make_unique<AssetBuffer>();
        const auto assets = parse_meta(meta, *assets_storage);
        ASSERT_TRUE(assets.has_value()) << dex;
        for (const auto& asset : *assets) {
            if (asset.delisted) {
                continue;
            }
            ++live;
            const auto base = classified_dex(asset.name, *categories);
            EXPECT_NE(base.asset_class, AssetClass::Unknown) << asset.name << " pairs with nothing";
            EXPECT_FALSE(base.underlying.empty()) << asset.name;
        }
    }
    EXPECT_EQ(live, 150U);
}

TEST(HyperliquidHip3, AContextReadNamesItsDexAndOnlyTheMainDexOmitsIt) {
    std::string body;
    EXPECT_EQ(RequestBuilder::meta_and_asset_ctxs("io", body).body, R"({"type":"metaAndAssetCtxs","dex":"io"})");
    EXPECT_EQ(RequestBuilder::meta_and_asset_ctxs("", body).body, RequestBuilder::meta_and_asset_ctxs().body);
    EXPECT_EQ(dex_of("io:OAI"), "io");
    EXPECT_TRUE(dex_of("BTC").empty());
}

TEST(HyperliquidHip3, EveryLiveDexContextPublishesFundingAndVolume) {
    const auto table = dex_table("rest_meta_dex_io_live.json");
    auto storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs_dex_io_live.json");
    const auto ctxs = parse_asset_ctxs(body, *storage);
    ASSERT_TRUE(ctxs.has_value());
    ASSERT_EQ(ctxs->size(), 11U);
    EXPECT_EQ((*ctxs)[0].coin, "io:OAI");
    std::size_t published = 0;
    for (const auto& ctx : *ctxs) {
        const auto funding = map_funding(ctx, table, UnixNanos{7});
        if (!funding.has_value()) {
            continue; // a delisted row is not in the table
        }
        EXPECT_FALSE(funding->mark_price.is_undef()) << ctx.coin;
        EXPECT_EQ(funding->interval_s, k_funding_interval_s);
        EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
        EXPECT_TRUE(map_ticker_stats(ctx, table, UnixNanos{7}).has_value()) << ctx.coin;
        ++published;
    }
    EXPECT_EQ(published, 8U);
    const auto oai = map_funding((*ctxs)[0], table, UnixNanos{7});
    ASSERT_TRUE(oai.has_value());
    EXPECT_EQ(oai->rate.raw, 13'219); // 0.0000132191 an hour, truncated at 1e-9
}

TEST(HyperliquidHip3, AContextBodyWhoseRowsOutnumberItsUniverseIsRefused) {
    auto storage = std::make_unique<AssetCtxBuffer>();
    const auto body = fixture("rest_meta_and_asset_ctxs_dex_para_live.json");
    ASSERT_TRUE(parse_asset_ctxs(body, *storage).has_value());
    // drop the universe's first row so every context would shift onto the wrong coin
    auto shifted = body;
    const auto first = shifted.find(R"({"szDecimals")");
    const auto second = shifted.find(R"({"szDecimals")", first + 1);
    ASSERT_NE(second, std::string::npos);
    shifted.erase(first, second - first);
    EXPECT_FALSE(parse_asset_ctxs(shifted, *storage).has_value());
}

TEST(HyperliquidHip3, TheHip3PlanFitsUnderTheIpCapOnlyWithContextsPolled) {
    // live 2026-10-04: 178 main + 110 xyz + 29 para + 8 io + 5 mkts
    constexpr std::size_t live = 330;
    constexpr std::size_t ip_cap = 1000;
    const auto plan = [](std::initializer_list<exec::SubKind> kinds, MarketType market) {
        std::set<std::string_view> channels;
        for (const auto kind : kinds) {
            if (const auto channel = stream_channel(kind, market); !channel.empty()) {
                channels.insert(channel);
            }
        }
        return channels.size() * live;
    };
    using enum exec::SubKind;
    EXPECT_EQ(plan({Bbo, Trades, Depth, Funding}, MarketType::Perp), 990U);
    EXPECT_LE(plan({Bbo, Trades, Depth, Funding}, MarketType::Perp), ip_cap);
    // streaming the context too, as before, would have left trades out
    EXPECT_GT((plan({Bbo, Trades, Depth}, MarketType::Perp) + live), ip_cap);
}

} // namespace
} // namespace raikiri::venues::hyperliquid
