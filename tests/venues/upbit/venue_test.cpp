#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/upbit/map.hpp>
#include <raikiri/venues/upbit/upbit_venue.hpp>
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
constexpr UnixNanos k_ingest{1'791'306'002'000'000'000ULL};

// the catalogue as fetch_all builds it: the listing, sized by the ticker's last prices
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
    ASSERT_EQ(listed->size(), 8U);
    for (const auto& row : *listed) {
        auto instrument = map_instrument(row, prices.at(std::string{row.market}), VenueId::Upbit, k_now);
        ASSERT_TRUE(instrument.has_value()) << row.market;
        classify(VenueId::Upbit, *instrument);
        ASSERT_TRUE(table.add(*instrument).has_value()) << row.market;
    }
}

const InstrumentBase& base_named(const exec::InstrumentTable& table, std::string_view code) {
    const auto* instrument = table.find(code);
    EXPECT_NE(instrument, nullptr) << code;
    return base_of(*instrument);
}

TEST(UpbitVenue, RegistersSpotOnlyAndRefusesEveryOrder) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto refused = exec::make_venue({.venue = VenueId::Upbit, .market = MarketType::Perp}, sink, clock);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), exec::MakeError::UnsupportedMarket);

    auto adapter = exec::make_venue({.venue = VenueId::Upbit, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->caps.ws_order_entry);
    EXPECT_EQ(adapter->caps.user_stream, exec::UserStreamModel::None);
    ASSERT_TRUE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());
    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Upbit, MarketType::Spot, SymbolId{1});
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Spot).has_value());
}

TEST(UpbitCatalogue, KeepsTheWonAsQuoteAndLoadsTheTetherRate) {
    exec::InstrumentTable table;
    load(table);
    const auto& usdt = base_named(table, "KRW-USDT");
    EXPECT_EQ(usdt.local_symbol.view(), "USDT/KRW");
    EXPECT_EQ(usdt.id.market(), MarketType::Spot);
    EXPECT_EQ(market_key(usdt), "crypto:USDT/KRW");
    const auto& btc = base_named(table, "KRW-BTC");
    EXPECT_EQ(market_key(btc), "crypto:BTC/KRW");
    EXPECT_EQ(btc.tick_size, k_price_step);
    EXPECT_EQ(btc.lot_size, FixedE9::from_raw(10));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(market_key(base_named(table, "USDT-BTC")), "crypto:BTC/USD*");
    EXPECT_EQ(base_named(table, "BTC-ETH").local_symbol.view(), "ETH/BTC");
}

TEST(UpbitCatalogue, WidensTheLotWhereEightPlacesWouldOverflowABook) {
    exec::InstrumentTable table;
    load(table);
    // 0.00788 won a coin: a level of ten trillion won is 1.27e15 coins
    EXPECT_EQ(base_named(table, "KRW-SHIB").lot_size, FixedE9::from_raw(1'000'000));
    EXPECT_EQ(base_named(table, "BTC-XRP").lot_size, FixedE9::from_raw(10));
    EXPECT_EQ(lot_for("KRW", ""), FixedE9::from_raw(10));
    EXPECT_EQ(lot_for("KRW", "0.0004"), FixedE9::from_raw(10'000'000));
}

TEST(UpbitBook, ReadsExponentPricesIntoTheWholeTopOfTheBook) {
    exec::InstrumentTable table;
    load(table);
    const auto& btc = base_named(table, "KRW-BTC");
    LevelBuffer levels;
    const auto frame = fixture("ws_orderbook_krw_btc.json");
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(parse_kind(frame), Kind::Orderbook);
    EXPECT_FALSE(wire->snapshot);
    const auto book = map_book(*wire, btc, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 30U);
    EXPECT_EQ(book->ask_count, 30U);
    EXPECT_EQ(book->bids[0].price.raw, 11'578'600'000'000'000);
    EXPECT_EQ(book->asks[0].price.raw, 11'583'200'000'000'000);
    EXPECT_EQ(book->bids[0].qty.raw, 1'000'000);
    EXPECT_EQ(book->asks[0].qty.raw, 1'677'399);
    EXPECT_EQ(book->ts_event, UnixNanos{1'791'306'001'766'000'000ULL});
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw);
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw);
    }
    const auto bbo = map_bbo(*wire, btc, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid, book->bids[0].price);
    EXPECT_EQ(bbo->ts_init, k_ingest);
}

TEST(UpbitBook, ACrossedOrUnorderedTopIsRefused) {
    exec::InstrumentTable table;
    load(table);
    const auto& btc = base_named(table, "KRW-BTC");
    LevelBuffer levels;
    auto frame = fixture("ws_orderbook_krw_btc.json");
    const auto at = frame.find("\"bid_price\":1.15786E8");
    ASSERT_NE(at, std::string::npos);
    frame.replace(at, 21, "\"bid_price\":1.15900E8");
    const auto crossed = parse_book(frame, levels);
    ASSERT_TRUE(crossed.has_value());
    EXPECT_FALSE(map_book(*crossed, btc, k_ingest).has_value());
}

TEST(UpbitBook, DropsDustBelowAWidenedLotAndReadsTheBtcMarket) {
    exec::InstrumentTable table;
    load(table);
    LevelBuffer levels;
    const auto shib_frame = fixture("ws_orderbook_krw_shib.json");
    const auto shib = parse_book(shib_frame, levels);
    ASSERT_TRUE(shib.has_value());
    const auto book = map_book(*shib, base_named(table, "KRW-SHIB"), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bids[0].price.raw, 787'000);
    EXPECT_EQ(book->bids[0].qty.raw, 6'280'975'458'758);

    const auto eth_frame = fixture("ws_orderbook_btc_eth.json");
    const auto eth = parse_book(eth_frame, levels);
    ASSERT_TRUE(eth.has_value());
    const auto top = map_bbo(*eth, base_named(table, "BTC-ETH"), k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid.raw, 3'147'908);
    EXPECT_EQ(top->ask.raw, 3'152'600);
}

TEST(UpbitTrade, ReadsTheAggressorAndFlagsTheSubscribeSnapshot) {
    exec::InstrumentTable table;
    load(table);
    const auto realtime_frame = fixture("ws_trade_realtime.json");
    const auto realtime = parse_trade(realtime_frame);
    ASSERT_TRUE(realtime.has_value());
    EXPECT_FALSE(realtime->snapshot);
    const auto trade = map_trade(*realtime, base_named(table, realtime->code), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price.raw, 364'800'000'000'000);
    EXPECT_EQ(trade->qty.raw, 3'090'000);
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'306'001'709'000'000ULL});

    const auto snapshot_frame = fixture("ws_trade_snapshot.json");
    const auto snapshot = parse_trade(snapshot_frame);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    const auto xrp_frame = fixture("ws_trade_krw_xrp.json");
    const auto xrp = parse_trade(xrp_frame);
    ASSERT_TRUE(xrp.has_value());
    EXPECT_TRUE(xrp->aggressor_bought);
}

TEST(UpbitTicker, CarriesWonTurnoverAndLeavesAnOverflowingBaseVolumeUndefined) {
    exec::InstrumentTable table;
    load(table);
    auto rows = std::make_unique<TickerBuffer>();
    const auto body = fixture("rest_ticker.json");
    const auto tickers = parse_tickers(body, *rows);
    ASSERT_TRUE(tickers.has_value());
    for (const auto& row : *tickers) {
        const auto stats = map_ticker_stats(row, base_named(table, row.market), true, k_ingest);
        ASSERT_TRUE(stats.has_value()) << row.market;
        if (row.market == "KRW-BTC") {
            EXPECT_EQ(stats->volume_24h_quote.raw, 88'940'129'887'670'780);
            EXPECT_EQ(stats->volume_24h_base, parse_decimal_e9("765.51242656").value());
            EXPECT_TRUE(stats->open_interest.is_undef());
            EXPECT_EQ(stats->ts_event, UnixNanos{1'791'306'071'000'000'000ULL});
        }
        if (row.market == "KRW-SHIB") {
            EXPECT_TRUE(stats->volume_24h_base.is_undef());
            EXPECT_GT(stats->volume_24h_quote.raw, 0);
        }
    }
    const auto unstamped = map_ticker_stats((*tickers)[0], base_named(table, (*tickers)[0].market), false, k_ingest);
    ASSERT_TRUE(unstamped.has_value());
    EXPECT_EQ(unstamped->ts_event, UnixNanos{});
}

TEST(UpbitWire, RefusalsReadOnBothTransports) {
    EXPECT_EQ(parse_kind(fixture("ws_error.json")), Kind::Refusal);
    const auto socket_frame = fixture("ws_error.json");
    const auto socket = parse_refusal(socket_frame);
    ASSERT_TRUE(socket.has_value());
    EXPECT_EQ(socket->name, "INVALID_PARAM");
    const auto rest_body = fixture("rest_error_unknown_market.json");
    const auto rest = parse_refusal(rest_body);
    ASSERT_TRUE(rest.has_value());
    EXPECT_EQ(rest->name, "404");
    EXPECT_FALSE(parse_refusal(fixture("rest_ticker.json")).has_value());
}

TEST(UpbitWire, StampsInMillisecondsOrMicroseconds) {
    EXPECT_EQ(stamp(0), UnixNanos{});
    EXPECT_EQ(stamp(1'791'305'711'581ULL), UnixNanos{1'791'305'711'581'000'000ULL});
    EXPECT_EQ(stamp(1'791'305'711'581'055ULL), UnixNanos{1'791'305'711'581'055'000ULL});
}

} // namespace
} // namespace raikiri::venues::upbit
