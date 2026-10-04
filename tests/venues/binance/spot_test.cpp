#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::binance {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string data_of(std::string_view name) {
    const auto frame = fixture(name);
    const auto inner = unwrap(frame);
    EXPECT_TRUE(inner.has_value());
    return std::string{*inner};
}

std::vector<Instrument> spot_catalogue() {
    static const auto storage = std::make_unique<InstrumentBuffer>();
    static const auto body = fixture("spot_rest_exchange_info.json");
    const auto rows = parse_exchange_info(body, *storage);
    EXPECT_TRUE(rows.has_value());
    std::vector<Instrument> out;
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Spot); instrument.has_value()) {
            out.push_back(*instrument);
        }
    }
    return out;
}

exec::InstrumentTable spot_table() {
    exec::InstrumentTable table;
    for (const auto& instrument : spot_catalogue()) {
        EXPECT_TRUE(table.add(instrument).has_value());
    }
    return table;
}

constexpr UnixNanos k_ingest{1'791'116'247'000'000'000ULL};

TEST(BinanceSpot, TheCatalogueKeepsOnlyTradingPairs) {
    const auto listed = spot_catalogue();
    std::vector<std::string_view> symbols;
    for (const auto& instrument : listed) {
        ASSERT_TRUE(std::holds_alternative<SpotInstrument>(instrument));
        EXPECT_EQ(base_of(instrument).id.market(), MarketType::Spot);
        symbols.push_back(base_of(instrument).exchange_symbol.view());
    }
    // NEOBTC is BREAK
    EXPECT_EQ(symbols, (std::vector<std::string_view>{"BTCUSDT", "ETHBTC", "币安人生USDT"}));
}

TEST(BinanceSpot, FiltersComeFromPriceLotAndNotional) {
    const auto listed = spot_catalogue();
    ASSERT_FALSE(listed.empty());
    const auto& btc = base_of(listed.front());
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(btc.tick_size.raw, 10'000'000);
    EXPECT_EQ(btc.lot_size.raw, 10'000);
    EXPECT_EQ(btc.min_notional.raw, 5'000'000'000);
}

TEST(BinanceSpot, NeitherMarketTakesTheOthersRows) {
    const auto storage = std::make_unique<InstrumentBuffer>();
    const auto futures = fixture("rest_exchange_info.json");
    const auto perp_rows = parse_exchange_info(futures, *storage);
    ASSERT_TRUE(perp_rows.has_value());
    for (const auto& row : *perp_rows) {
        EXPECT_FALSE(map_instrument(row, MarketType::Spot).has_value()) << row.symbol;
    }
    const auto spot = fixture("spot_rest_exchange_info.json");
    const auto spot_rows = parse_exchange_info(spot, *storage);
    ASSERT_TRUE(spot_rows.has_value());
    for (const auto& row : *spot_rows) {
        EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value()) << row.symbol;
    }
}

TEST(BinanceSpot, ABookTickerWithNoEventTypeIsStillABookTicker) {
    const auto frame = fixture("spot_ws_book_ticker.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::BookTicker);
    EXPECT_EQ(event->symbol, "BTCUSDT");

    const auto ack = parse_event(R"({"result":null,"id":1})");
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::SubscribeAck);
}

TEST(BinanceSpot, BboTakesTheLocalTimeSinceSpotStatesNone) {
    const auto table = spot_table();
    const auto data = data_of("spot_ws_book_ticker.json");
    const auto wire = parse_book_ticker(data);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->update_id, 101'022'247'046ULL);
    const auto bbo = map_bbo(*wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 8'518'992);
    EXPECT_EQ(bbo->ask.raw, 8'518'993);
    EXPECT_EQ(bbo->bid_qty.raw, 853'244);
    EXPECT_EQ(bbo->ask_qty.raw, 124'122);
    EXPECT_EQ(bbo->ts_event, k_ingest);
}

TEST(BinanceSpot, TradeKeepsTheVenueTradeTime) {
    const auto table = spot_table();
    const auto frame = fixture("spot_ws_trade.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Trade);
    const auto data = data_of("spot_ws_trade.json");
    const auto wire = parse_trade(data);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->trade_id, 6'734'421'578ULL);
    const auto trade = map_trade(*wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 8'518'993);
    EXPECT_EQ(trade->qty.raw, 29);
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'791'116'246'948ULL * 1'000'000ULL);
}

struct SpotBook {
    DepthBook book;
    LevelBuffer storage;

    static std::unique_ptr<SpotBook> make() {
        const auto table = spot_table();
        const auto* btc = table.find(std::string_view{"BTCUSDT"});
        EXPECT_NE(btc, nullptr);
        const auto& base = base_of(*btc);
        return std::make_unique<SpotBook>(DepthBook{base.id, base.tick_size, base.lot_size});
    }

    BookResult seed() {
        const auto body = fixture("spot_rest_depth_seed.json");
        const auto wire = parse_depth_snapshot(body, storage);
        EXPECT_TRUE(wire.has_value());
        return book.seed(*wire);
    }

    BookResult apply(std::string_view name) {
        const auto data = data_of(name);
        const auto wire = parse_depth_update(data, storage);
        EXPECT_TRUE(wire.has_value());
        return book.apply(*wire);
    }
};

TEST(BinanceSpot, DepthChainsOnContiguousIds) {
    LevelBuffer storage;
    const auto data = data_of("spot_ws_depth_update.json");
    const auto wire = parse_depth_update(data, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->first_id, 101'022'247'333ULL);
    EXPECT_EQ(wire->prev_id, 101'022'247'332ULL);
}

TEST(BinanceSpot, TheEventRightAfterTheSeedBridgesIt) {
    const auto book = SpotBook::make();
    EXPECT_EQ(book->seed(), BookResult::Waiting);
    EXPECT_EQ(book->apply("spot_ws_depth_update.json"), BookResult::Applied);
    EXPECT_EQ(book->apply("spot_ws_depth_update_next.json"), BookResult::Applied);
    EXPECT_EQ(book->book.last_update_id(), 101'022'247'360ULL);
    EXPECT_EQ(book->book.gaps(), 0U);
}

TEST(BinanceSpot, AnEventTheSeedAlreadyHoldsAlsoBridgesIt) {
    const auto book = SpotBook::make();
    EXPECT_EQ(book->apply("spot_ws_depth_contained.json"), BookResult::Buffered);
    EXPECT_EQ(book->apply("spot_ws_depth_update.json"), BookResult::Buffered);
    EXPECT_EQ(book->seed(), BookResult::Applied);
    EXPECT_EQ(book->book.last_update_id(), 101'022'247'346ULL);
    ASSERT_FALSE(book->book.bids().empty());
    EXPECT_EQ(book->book.bids().front().price.raw, 8'518'992);
}

TEST(BinanceSpot, AMissedEventDropsTheBook) {
    const auto book = SpotBook::make();
    (void)book->seed();
    EXPECT_EQ(book->apply("spot_ws_depth_contained.json"), BookResult::Applied);
    EXPECT_EQ(book->apply("spot_ws_depth_update_next.json"), BookResult::Gap);
    EXPECT_FALSE(book->book.live());
}

TEST(BinanceSpot, TickerStatsReadTheSpotTable) {
    const auto table = spot_table();
    const auto storage = std::make_unique<Ticker24hBuffer>();
    const auto body = fixture("spot_rest_ticker_24h.json");
    const auto rows = parse_ticker_24h(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto stats = map_ticker_stats(rows->front(), table, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 7'291'667'610'000);
    EXPECT_EQ(stats->volume_24h_quote.raw / 1'000'000, 619'172'111);
    EXPECT_EQ(stats->ts_event.ns, 1'791'116'277'012ULL * 1'000'000ULL);
    // a halted pair is not in the catalogue, so its row reaches nobody
    EXPECT_FALSE(map_ticker_stats((*rows)[2], table, k_ingest).has_value());
}

TEST(BinanceSpot, SpotUsesItsOwnHostsAndPaths) {
    EXPECT_EQ(rest_host(false, MarketType::Spot), "api.binance.com");
    EXPECT_EQ(stream_host(false, MarketType::Spot), "stream.binance.com");
    EXPECT_EQ(rest_host(false), "fapi.binance.com");

    const net::Signer signer;
    RequestBuilder spot{signer, MarketType::Spot};
    EXPECT_TRUE(spot.instruments().target.starts_with("/api/v3/exchangeInfo?symbolStatus=TRADING"));
    EXPECT_EQ(spot.ticker_24h().target, "/api/v3/ticker/24hr");
    EXPECT_EQ(spot.server_time().target, "/api/v3/time");
    EXPECT_EQ(spot.depth_snapshot("BTCUSDT", k_seed_depth).target, "/api/v3/depth?symbol=BTCUSDT&limit=100");
}

TEST(BinanceSpot, SpotStreamsBboTradesAndDepthOnly) {
    std::array<char, 64> buf{};
    EXPECT_EQ(stream_name(exec::SubKind::Bbo, "BTCUSDT", buf, MarketType::Spot), "btcusdt@bookTicker");
    EXPECT_EQ(stream_name(exec::SubKind::Trades, "BTCUSDT", buf, MarketType::Spot), "btcusdt@trade");
    EXPECT_EQ(stream_name(exec::SubKind::Depth, "BTCUSDT", buf, MarketType::Spot), "btcusdt@depth");
    EXPECT_TRUE(stream_name(exec::SubKind::Funding, "BTCUSDT", buf, MarketType::Spot).empty());
    EXPECT_TRUE(stream_name(exec::SubKind::MarkPrice, "BTCUSDT", buf, MarketType::Spot).empty());
    EXPECT_EQ(stream_name(exec::SubKind::Depth, "BTCUSDT", buf), "btcusdt@depth@500ms");
}

} // namespace
} // namespace raikiri::venues::binance
