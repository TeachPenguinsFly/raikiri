// aster parser tests against captured bytes; see fixtures/README.md

#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/catalogue.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/envelope.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/market_data.hpp>
#include <raikiri/venues/aster/rest.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace raikiri::venues::aster {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// every parser takes the payload, not the {"stream":..,"data":..} wrapper
std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    WireInstrument wire{};
    wire.symbol = "BTCUSDT";
    wire.base_asset = "BTC";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.min_notional = "5";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

TEST(AsterWire, ExchangeInfoParsesTheThreeShapesTheVenueServes) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    // the fixture keeps one plain perp, one non-usdt quote and one not trading
    ASSERT_EQ(rows->size(), 3U);

    bool saw_btc = false;
    for (const auto& row : *rows) {
        if (row.symbol == "BTCUSDT") {
            saw_btc = true;
            EXPECT_EQ(row.contract_type, "PERPETUAL");
            EXPECT_EQ(row.quote_asset, "USDT");
        }
    }
    EXPECT_TRUE(saw_btc);
}

TEST(AsterMap, ANonUsdtQuoteSurvivesTheMapper) {
    // rows quoted in USD1 and "U" must map too
    WireInstrument wire{};
    wire.symbol = "BTCUSD1";
    wire.base_asset = "BTC";
    wire.quote_asset = "USD1";
    wire.contract_type = "PERPETUAL";
    wire.status = "TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
}

TEST(AsterMap, ARowThatIsNotTradingIsRefused) {
    WireInstrument wire{};
    wire.symbol = "PENDINGUSDT";
    wire.base_asset = "PENDING";
    wire.quote_asset = "USDT";
    wire.contract_type = "PERPETUAL";
    wire.status = "PENDING_TRADING";
    wire.tick_size = "0.10";
    wire.step_size = "0.001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(AsterWire, EventDispatchResolvesKindAndSymbol) {
    for (const auto& [file, kind] : {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
                                     std::pair{"ws_agg_trade.json", EventKind::Trade},
                                     std::pair{"ws_mark_price.json", EventKind::MarkPrice},
                                     std::pair{"ws_depth_update.json", EventKind::DepthUpdate}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        EXPECT_EQ(event->symbol, "BTCUSDT") << file;
    }
}

TEST(AsterMap, BookTickerBecomesBbo) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_book_ticker.json");
    const auto wire = parse_book_ticker(payload);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_GT(bbo->bid.raw, 0);
    EXPECT_GT(bbo->ask.raw, bbo->bid.raw);
    EXPECT_NE(bbo->ts_event.ns, 0ULL);
}

TEST(AsterMap, MarkPriceCarriesTheFundingRate) {
    const auto table = table_with_btc();
    const auto payload = data_of("ws_mark_price.json");
    const auto wire = parse_mark_price(payload);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // the mapper may refuse a contract quoting no rate; this fixture quotes one
    EXPECT_NE(funding->next_funding_time.ns, 0ULL);
    EXPECT_EQ(funding->mark_price.raw, 803'296);  // 80329.60794203 truncated onto the 0.10 tick
    EXPECT_EQ(funding->index_price.raw, 803'590); // 80359.00804348, likewise
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->ts_event.ns, 1'789'901'124'000'000'000ULL);
}

TEST(AsterBook, ConsecutiveDepthFramesChainByPrevUpdateId) {
    // frame N's `u` is frame N+1's `pu`
    const auto first = data_of("ws_depth_update.json");
    const auto second = data_of("ws_depth_update_next.json");
    LevelBuffer buf_a;
    LevelBuffer buf_b;
    const auto a = parse_depth_update(first, buf_a);
    const auto b = parse_depth_update(second, buf_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->last_id, b->prev_id);
    EXPECT_LE(a->first_id, a->last_id);
}

TEST(AsterEndpoints, TestnetHasNowhereToPointSoBothAccessorsAnswerLive) {
    // no testnet host resolves
    EXPECT_EQ(ep::rest_host(true), ep::k_rest_live);
    EXPECT_EQ(ep::stream_host(true), ep::k_stream_live);
}

TEST(AsterMap, TheSubTypeTellsAStockFromACoin) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info_tradfi.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    std::map<std::string, std::string, std::less<>> keys;
    for (const auto& row : *rows) {
        auto instrument = map_instrument(row, MarketType::Perp);
        ASSERT_TRUE(instrument.has_value()) << row.symbol;
        classify(VenueId::Aster, *instrument);
        keys.emplace(std::string{row.symbol}, asset_key(base_of(*instrument)));
    }
    EXPECT_EQ(keys["BTCUSDT"], "crypto:BTC");
    EXPECT_EQ(keys["AAPLUSDT"], "equity:AAPL");
    EXPECT_EQ(keys["IWMUSDT"], "equity:IWM");
    EXPECT_EQ(keys["XAGUSDT"], "commodity:XAG");
}

exec::InstrumentTable spot_table() {
    static thread_local InstrumentBuffer storage;
    static const auto body = fixture("spot_rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Spot); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value()) << row.symbol;
        }
    }
    return table;
}

TEST(AsterSpot, TheSpotCatalogueLoadsAsSpotAndNeverAsPerp) {
    static thread_local InstrumentBuffer storage;
    const auto body = fixture("spot_rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    for (const auto& row : *rows) {
        EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value()) << row.symbol;
    }
    const auto table = spot_table();
    EXPECT_EQ(table.size(), 4U);
    const auto* btc = table.find("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(*btc));
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.id.market(), MarketType::Spot);
    EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base.tick_size.raw, 10'000'000);        // 0.01
    EXPECT_EQ(base.lot_size.raw, 10'000);             // 0.00001
    EXPECT_EQ(base.min_notional.raw, 10'000'000'000); // spot spells it minNotional
    EXPECT_EQ(base.asset_class, AssetClass::Crypto);
    EXPECT_EQ(base_of(*table.find("WLFIUSD1")).local_symbol.view(), "WLFI/USD1");
}

TEST(AsterSpot, ThePerpCatalogueStillReadsItsOwnMinimum) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto perp = std::ranges::find_if(*rows, [](const WireInstrument& row) { return !row.min_notional.empty(); });
    EXPECT_NE(perp, rows->end());
}

TEST(AsterSpot, QuotesPrintsAndBooksShareTheFuturesShapes) {
    const auto table = spot_table();
    const auto quote_payload = data_of("spot_ws_book_ticker.json");
    const auto quote = parse_book_ticker(quote_payload);
    ASSERT_TRUE(quote.has_value());
    const auto bbo = map_bbo(*quote, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 8'659'152); // 86591.52 / 0.01
    EXPECT_EQ(bbo->ask.raw, 8'659'239);
    EXPECT_NE(bbo->ts_event.ns, 0ULL);

    const auto print_payload = data_of("spot_ws_trade.json");
    const auto print = parse_trade(print_payload);
    ASSERT_TRUE(print.has_value());
    const auto trade = map_trade(*print, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 2'800); // 0.2800 / 0.0001
    EXPECT_EQ(trade->qty.raw, 422);     // 42.20 base / 0.10
    EXPECT_EQ(trade->aggressor, Side::Sell);

    LevelBuffer buf_a;
    LevelBuffer buf_b;
    const auto first = data_of("spot_ws_depth_update.json");
    const auto second = data_of("spot_ws_depth_update_next.json");
    const auto a = parse_depth_update(first, buf_a);
    const auto b = parse_depth_update(second, buf_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->last_id, b->prev_id);
    LevelBuffer seed;
    const auto seed_body = fixture("spot_rest_depth_snapshot.json");
    EXPECT_TRUE(parse_depth_snapshot(seed_body, seed).has_value());
}

TEST(AsterSpot, TheDailyTableCarriesBothVolumeColumns) {
    const auto table = spot_table();
    static thread_local Ticker24hBuffer storage;
    const auto body = fixture("spot_rest_ticker_24h.json");
    const auto rows = parse_ticker_24h(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto btc = std::ranges::find_if(*rows, [](const WireTicker24h& row) { return row.symbol == "BTCUSDT"; });
    ASSERT_NE(btc, rows->end());
    const auto stats = map_ticker_stats(*btc, table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->instrument.market(), MarketType::Spot);
    EXPECT_EQ(stats->volume_24h_base.raw, 4'735'920'000);    // 4.73592
    EXPECT_EQ(stats->volume_24h_quote.raw, 406'446'123'515); // 406446.123515
    EXPECT_EQ(stats->ts_event.ns, 1'791'299'537'187'000'000ULL);
}

TEST(AsterSpot, SpotAddressesItsOwnHostsAndPaths) {
    EXPECT_EQ(rest_host(false, MarketType::Spot), ep::k_spot_rest_live);
    EXPECT_EQ(stream_host(false, MarketType::Spot), ep::k_spot_stream_live);
    EXPECT_EQ(rest_host(false), ep::k_rest_live);
    const net::Signer signer;
    RequestBuilder spot{signer, MarketType::Spot};
    EXPECT_EQ(spot.instruments().target, ep::k_spot_exchange_info);
    EXPECT_EQ(spot.ticker_24h().target, ep::k_spot_ticker_24h);
    EXPECT_EQ(spot.server_time().target, ep::k_spot_server_time);
    EXPECT_TRUE(spot.depth_snapshot("BTCUSDT", 1000).target.starts_with(ep::k_spot_depth));
    RequestBuilder perp{signer};
    EXPECT_EQ(perp.instruments().target, ep::k_exchange_info);
    const auto time_body = fixture("spot_rest_time.json");
    const auto now = parse_server_time(time_body);
    ASSERT_TRUE(now.has_value());
    EXPECT_GT(*now, 1'791'000'000'000ULL);
}

} // namespace
} // namespace raikiri::venues::aster
