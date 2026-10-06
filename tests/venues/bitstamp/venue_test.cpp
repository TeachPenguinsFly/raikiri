#include <raikiri/common/fixed_point.hpp>
#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/bitstamp/bitstamp_venue.hpp>
#include <raikiri/venues/bitstamp/map.hpp>
#include <raikiri/venues/bitstamp/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace raikiri::venues::bitstamp {
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

// the captures were taken on 2026-10-06 near 17:01 utc
constexpr UnixNanos k_now{1'791'306'000'000'000'000ULL};
constexpr UnixNanos k_ingest{1'791'306'100'000'000'000ULL};

std::size_t load(exec::InstrumentTable& table, MarketType market) {
    const auto body = fixture("rest_markets.json");
    auto rows = std::make_unique<MarketBuffer>();
    const auto listed = parse_markets(body, *rows);
    EXPECT_TRUE(listed.has_value());
    std::size_t loaded = 0;
    for (const auto& row : *listed) {
        auto instrument = map_instrument(row, market, k_now);
        if (!instrument.has_value()) {
            continue;
        }
        classify(VenueId::Bitstamp, *instrument);
        loaded += table.add(*instrument).has_value() ? 1U : 0U;
    }
    return loaded;
}

const InstrumentBase& base_named(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return base_of(*instrument);
}

TEST(BitstampVenue, ServesSpotAndPerpetualsAndRefusesEveryOrder) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    for (const auto market : {MarketType::Spot, MarketType::Perp}) {
        EXPECT_TRUE(exec::make_venue({.venue = VenueId::Bitstamp, .market = market}, sink, clock).has_value());
    }
    const auto refused = exec::make_venue({.venue = VenueId::Bitstamp, .market = MarketType::Option}, sink, clock);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), exec::MakeError::UnsupportedMarket);

    auto adapter = exec::make_venue({.venue = VenueId::Bitstamp, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_FALSE(adapter->caps.ws_order_entry);
    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Bitstamp, MarketType::Perp, SymbolId{1});
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
}

TEST(BitstampCatalogue, SpotTakesStepsFromDecimalsAndFiatPairsAreFx) {
    exec::InstrumentTable table;
    EXPECT_EQ(load(table, MarketType::Spot), 5U);
    const auto& btc = base_named(table, "btcusd");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(btc.tick_size, FixedE9::from_raw(10'000'000));
    EXPECT_EQ(btc.lot_size, FixedE9::from_raw(10));
    EXPECT_EQ(btc.min_notional, parse_decimal_e9("10").value());
    EXPECT_EQ(market_key(btc), "crypto:BTC/USD");
    EXPECT_EQ(market_key(base_named(table, "usdtusd")), "crypto:USDT/USD");
    EXPECT_EQ(market_key(base_named(table, "eurusd")), "fx:EURUSD/USD");
    EXPECT_EQ(base_named(table, "ethbtc").tick_size, FixedE9::from_raw(10));
}

TEST(BitstampCatalogue, PerpetualsTakeTheClassTheVenueStates) {
    exec::InstrumentTable table;
    EXPECT_EQ(load(table, MarketType::Perp), 9U);
    const auto* btc = table.find("btcusd-perp");
    ASSERT_NE(btc, nullptr);
    const auto* perp = std::get_if<PerpInstrument>(btc);
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->funding_interval_s, k_funding_interval_s);
    EXPECT_EQ(perp->base.tick_size, FixedE9::from_raw(k_e9));
    EXPECT_EQ(perp->base.lot_size, FixedE9::from_raw(10'000));
    EXPECT_EQ(perp->base.contract_value, FixedE9::from_raw(k_e9));
    EXPECT_EQ(asset_key(base_named(table, "goldusd-perp")), "commodity:XAU");
    EXPECT_EQ(asset_key(base_named(table, "silverusd-perp")), "commodity:XAG");
    EXPECT_EQ(asset_key(base_named(table, "wtiusd-perp")), "commodity:CL");
    EXPECT_EQ(asset_key(base_named(table, "brentusd-perp")), "commodity:BRENT");
    EXPECT_EQ(asset_key(base_named(table, "qqqusd-perp")), "equity:QQQ");
    EXPECT_EQ(asset_key(base_named(table, "ewyusd-perp")), "equity:EWY");
    EXPECT_EQ(asset_key(base_named(table, "eurusd-perp")), "fx:EURUSD");
    EXPECT_EQ(asset_key(base_named(table, "paxgusd-perp")), "crypto:PAXG");
}

TEST(BitstampBook, EachFrameIsTheWholeTopHundred) {
    exec::InstrumentTable table;
    load(table, MarketType::Spot);
    const auto frame = fixture("ws_order_book_btcusd.json");
    const auto head = parse_head(frame);
    EXPECT_EQ(head.event, Event::Book);
    EXPECT_EQ(symbol_of(head.channel), "btcusd");
    LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    const auto book = map_book(*wire, base_named(table, "btcusd"), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 100U);
    EXPECT_EQ(book->ask_count, 100U);
    EXPECT_EQ(book->bids[0].price.raw, 8'560'719);
    EXPECT_EQ(book->asks[0].price.raw, 8'560'720);
    EXPECT_EQ(book->asks[0].qty.raw, 67'377'952);
    EXPECT_EQ(book->ts_event, UnixNanos{1'791'306'088'206'800'000ULL});

    const auto thin_frame = fixture("ws_order_book_usdtusd.json");
    const auto thin = parse_book(thin_frame, levels);
    ASSERT_TRUE(thin.has_value());
    const auto usdt = map_book(*thin, base_named(table, "usdtusd"), k_ingest);
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->bid_count, 24U);
    EXPECT_EQ(usdt->bids[0].price.raw, 99'988);
}

TEST(BitstampBook, ACrossedBookIsRefused) {
    exec::InstrumentTable table;
    load(table, MarketType::Perp);
    auto frame = fixture("ws_order_book_btcusd_perp.json");
    const auto at = frame.find(R"(["85619",)");
    ASSERT_NE(at, std::string::npos);
    frame.replace(at, 9, R"(["85630",)");
    LevelBuffer levels;
    const auto wire = parse_book(frame, levels);
    ASSERT_TRUE(wire.has_value());
    EXPECT_FALSE(map_book(*wire, base_named(table, "btcusd-perp"), k_ingest).has_value());
}

TEST(BitstampTrade, ReadsTheAggressorAndTheMicrosecondStamp) {
    exec::InstrumentTable table;
    load(table, MarketType::Spot);
    const auto frame = fixture("ws_live_trades_btcusd.json");
    EXPECT_EQ(parse_head(frame).event, Event::Trade);
    const auto wire = parse_trade(frame);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(*wire, base_named(table, "btcusd"), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 8'561'375);
    EXPECT_EQ(trade->qty.raw, 231'417);
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'306'098'407'000'000ULL});
}

TEST(BitstampFunding, CarriesRateMarkIndexAndTheNextInstant) {
    exec::InstrumentTable table;
    load(table, MarketType::Perp);
    const auto frame = fixture("ws_funding_rate_btcusd_perp.json");
    const auto head = parse_head(frame);
    EXPECT_EQ(head.event, Event::Funding);
    const auto wire = parse_funding(frame);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, base_named(table, symbol_of(head.channel)), k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, parse_decimal_e9("0.000037").value());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price.raw, 85'628);
    EXPECT_EQ(funding->index_price.raw, 85'613);
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'791'331'200'000'000'000ULL});
    EXPECT_EQ(funding->ts_event, UnixNanos{1'791'306'088'000'000'000ULL});

    const auto gold_frame = fixture("ws_funding_rate_goldusd_perp.json");
    const auto gold = parse_funding(gold_frame);
    ASSERT_TRUE(gold.has_value());
    const auto metal = map_funding(*gold, base_named(table, "goldusd-perp"), k_ingest);
    ASSERT_TRUE(metal.has_value());
    EXPECT_GT(metal->index_price.raw, 0);
}

TEST(BitstampTicker, TurnoverIsVolumeAtTheVwapAndPerpetualsCarryOpenInterest) {
    exec::InstrumentTable spot;
    load(spot, MarketType::Spot);
    exec::InstrumentTable perp;
    load(perp, MarketType::Perp);
    const auto body = fixture("rest_ticker.json");
    auto rows = std::make_unique<TickerBuffer>();
    const auto tickers = parse_tickers(body, *rows);
    ASSERT_TRUE(tickers.has_value());
    std::size_t seen = 0;
    for (const auto& row : *tickers) {
        if (row.pair == "BTC/USD") {
            const auto stats = map_ticker_stats(row, base_named(spot, "btcusd"), k_ingest);
            ASSERT_TRUE(stats.has_value());
            // 1151.69192541 at 85881.51
            EXPECT_EQ(stats->volume_24h_quote.raw, 98'909'041'609'018);
            EXPECT_TRUE(stats->open_interest.is_undef());
            EXPECT_EQ(stats->ts_event, UnixNanos{1'791'306'146'000'000'000ULL});
            ++seen;
        }
        if (row.pair == "EWY/USD-PERP") {
            const auto stats = map_ticker_stats(row, base_named(perp, "ewyusd-perp"), k_ingest);
            ASSERT_TRUE(stats.has_value());
            EXPECT_EQ(stats->open_interest, parse_decimal_e9("398.413").value());
            EXPECT_EQ(stats->volume_24h_base, parse_decimal_e9("909.759").value());
            ++seen;
        }
    }
    EXPECT_EQ(seen, 2U);
}

TEST(BitstampWire, SubscriptionAnswersAndRefusalsAreTold) {
    EXPECT_EQ(parse_head(fixture("ws_subscription_succeeded.json")).event, Event::Subscribed);
    EXPECT_EQ(parse_head(fixture("ws_error.json")).event, Event::Refused);
    EXPECT_EQ(parse_head(R"({"event":"bts:request_reconnect","channel":"","data":""})").event, Event::Reconnect);
    EXPECT_EQ(symbol_of("live_trades_btcusd-perp"), "btcusd-perp");
    EXPECT_EQ(symbol_of("bbo_btcusd"), "");
}

} // namespace
} // namespace raikiri::venues::bitstamp
