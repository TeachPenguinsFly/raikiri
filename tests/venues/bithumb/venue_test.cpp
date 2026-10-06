#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/bithumb/bithumb_venue.hpp>
#include <raikiri/venues/upbit/map.hpp>
#include <raikiri/venues/upbit/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::upbit {
namespace {

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// the captures were taken on 2026-10-06 near 17:00 utc
constexpr UnixNanos k_now{1'791'306'000'000'000'000ULL};
constexpr UnixNanos k_ingest{1'791'306'016'000'000'000ULL};

void load(exec::InstrumentTable& table) {
    const auto tickers_body = fixture("rest_ticker.json");
    auto tickers = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(tickers_body, *tickers);
    ASSERT_TRUE(rows.has_value());
    std::unordered_map<std::string, std::string> prices;
    for (const auto& row : *rows) {
        prices.emplace(std::string{row.market}, std::string{row.trade_price});
    }
    const auto listing = fixture("rest_market_all.json");
    auto markets = std::make_unique<MarketBuffer>();
    const auto listed = parse_markets(listing, *markets);
    ASSERT_TRUE(listed.has_value());
    ASSERT_EQ(listed->size(), 7U);
    for (const auto& row : *listed) {
        auto instrument = map_instrument(row, prices.at(std::string{row.market}), VenueId::Bithumb, k_now);
        ASSERT_TRUE(instrument.has_value()) << row.market;
        classify(VenueId::Bithumb, *instrument);
        ASSERT_TRUE(table.add(*instrument).has_value()) << row.market;
    }
}

const InstrumentBase& base_named(const exec::InstrumentTable& table, std::string_view code) {
    const auto* instrument = table.find(code);
    EXPECT_NE(instrument, nullptr) << code;
    return base_of(*instrument);
}

TEST(BithumbVenue, RegistersOnItsOwnHostsAndRefusesEveryOrder) {
    bithumb::register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    EXPECT_FALSE(exec::make_venue({.venue = VenueId::Bithumb, .market = MarketType::Perp}, sink, clock).has_value());
    auto adapter = exec::make_venue({.venue = VenueId::Bithumb, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bithumb);
    ASSERT_TRUE(adapter->clock.has_value());
    EXPECT_EQ(adapter->clock->query().host, "api.bithumb.com");
    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Bithumb, MarketType::Spot, SymbolId{1});
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
}

TEST(BithumbCatalogue, KeepsTheWonAsQuote) {
    exec::InstrumentTable table;
    load(table);
    EXPECT_EQ(market_key(base_named(table, "KRW-USDT")), "crypto:USDT/KRW");
    EXPECT_EQ(market_key(base_named(table, "KRW-BTC")), "crypto:BTC/KRW");
    EXPECT_EQ(base_named(table, "BTC-ETH").local_symbol.view(), "ETH/BTC");
    EXPECT_EQ(base_named(table, "KRW-BTC").id.venue(), VenueId::Bithumb);
}

TEST(BithumbBook, StampsInMicrosecondsAndSkipsEmptyLevels) {
    exec::InstrumentTable table;
    load(table);
    const auto frame = fixture("ws_orderbook_krw_btc.json");
    LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    const auto book = map_book(*wire, base_named(table, "KRW-BTC"), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->ts_event, UnixNanos{1'791'306'015'743'773'000ULL});
    EXPECT_EQ(book->bid_count + book->ask_count, 28U);
    EXPECT_EQ(book->bids[0].price.raw, 11'578'800'000'000'000);
    EXPECT_EQ(book->asks[0].qty.raw, 35'480'000);
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0);
    }
}

TEST(BithumbTrade, ReadsTheRealtimePrint) {
    exec::InstrumentTable table;
    load(table);
    const auto frame = fixture("ws_trade_realtime.json");
    const auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(wire->snapshot);
    const auto trade = map_trade(*wire, base_named(table, "KRW-USDT"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->qty.raw, 5'982'956'000);
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'306'016'257'000'000ULL});
}

TEST(BithumbTicker, TheRestTickerRunsNineHoursAheadSoItGoesUnstamped) {
    exec::InstrumentTable table;
    load(table);
    const auto body = fixture("rest_ticker.json");
    auto rows = std::make_unique<TickerBuffer>();
    const auto tickers = parse_tickers(body, *rows);
    ASSERT_TRUE(tickers.has_value());
    const auto& row = (*tickers)[0];
    ASSERT_EQ(row.market, "KRW-BTC");
    EXPECT_GT(stamp(row.ts_ms).ns, k_ingest.ns + 8ULL * 3'600'000'000'000ULL);
    const auto stats = map_ticker_stats(row, base_named(table, "KRW-BTC"), bithumb::k_flavour.ticker_stamped, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->ts_event, UnixNanos{});
    EXPECT_EQ(stats->volume_24h_quote.raw, 24'892'050'434'941'410);
}

} // namespace
} // namespace raikiri::venues::upbit
