#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/bitunix/endpoints.hpp>
#include <raikiri/venues/bitunix/map.hpp>
#include <raikiri/venues/bitunix/wire.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::bitunix {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'964'213'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

WirePair pair(std::string_view symbol) {
    static const auto body = fixture("rest_trading_pairs.json");
    static const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no pair " << symbol;
    return {};
}

InstrumentBase btc() {
    const auto instrument = map_pair(pair("BTCUSDT"), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Bitunix, MarketType::Perp, SymbolId{2});
    return base;
}

TEST(BitunixCatalogue, LoadsOnlyOpenLinearContractsTheApiTrades) {
    const auto body = fixture("rest_trading_pairs.json");
    const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_pairs(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 7U);

    std::size_t loaded = 0;
    for (const auto& one : *rows) {
        loaded += map_pair(one, MarketType::Perp).has_value() ? 1U : 0U;
    }
    EXPECT_EQ(loaded, 4U);
    EXPECT_FALSE(pair("QQQUSDT").api);
    EXPECT_FALSE(map_pair(pair("QQQUSDT"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_pair(pair("FDUSDUSDT"), MarketType::Perp).has_value()) << "PREVIEW";
    EXPECT_FALSE(map_pair(pair("BTCUSD"), MarketType::Perp).has_value()) << "coin-margined";

    const auto perp = map_pair(pair("BTCUSDT"), MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    const auto& base = base_of(*perp);
    EXPECT_EQ(base.exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base.tick_size, e9("0.1"));
    EXPECT_EQ(base.lot_size, e9("0.0001"));
    EXPECT_EQ(base.kind, ContractKind::Linear);
    const auto& row = std::get<PerpInstrument>(*perp);
    EXPECT_EQ(row.funding_cap, e9("0.003"));
    EXPECT_EQ(row.funding_floor, e9("-0.003"));
}

TEST(BitunixCatalogue, SpotLoadsOpenPairsOnly) {
    const auto body = fixture("spot_rest_coin_pairs.json");
    const auto status = parse_status(body);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, k_ok) << "the spot host states its code as a string";

    const auto storage = std::make_unique<PairBuffer>();
    const auto rows = parse_spot_pairs(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto spot = map_pair((*rows)[0], MarketType::Spot);
    ASSERT_TRUE(spot.has_value());
    EXPECT_TRUE(std::holds_alternative<SpotInstrument>(*spot));
    EXPECT_EQ(base_of(*spot).exchange_symbol.view(), "BTCUSDT");
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base_of(*spot).tick_size, e9("0.01"));
    EXPECT_EQ(base_of(*spot).lot_size, e9("0.00001"));
    EXPECT_FALSE(map_pair((*rows)[2], MarketType::Spot).has_value()) << "closed";
}

TEST(BitunixCatalogue, SpotDepthIsPolledAtTheTickPrecision) {
    const auto body = fixture("spot_rest_depth.json");
    LevelBuffer storage;
    const auto wire = parse_spot_depth(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());
    EXPECT_EQ(wire->asks.front().price, "84854.01");
    EXPECT_EQ(wire->asks.front().qty, "0.1027");
    EXPECT_EQ(precision_text(e9("0.01")), "0.01");
    EXPECT_EQ(precision_text(e9("1")), "1");
    EXPECT_EQ(precision_text(e9("0.00001")), "0.00001");
}

TEST(BitunixRest, OverloadIsAnHttp200WithCode100009) {
    const auto body = fixture("rest_rate_limited.json");
    const auto status = parse_status(body);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, k_rate_limited);
    EXPECT_EQ(status->msg, "System error");
}

TEST(BitunixMap, PercentRatesBecomeFractions) {
    EXPECT_EQ(percent_to_fraction("0.3"), e9("0.003"));
    EXPECT_EQ(percent_to_fraction("-0.001352"), e9("-0.00001352"));
    EXPECT_EQ(step_of(0), e9("1"));
    EXPECT_EQ(step_of(4), e9("0.0001"));
    EXPECT_FALSE(step_of(10).has_value());
}

TEST(BitunixFunding, TheBatchRateIsPercentAndCarriesItsOwnInterval) {
    const auto body = fixture("rest_funding_batch.json");
    const auto storage = std::make_unique<FundingBuffer>();
    const auto rows = parse_funding(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    const auto row = std::ranges::find(*rows, std::string_view{"BTCUSDT"}, &WireFunding::symbol);
    ASSERT_NE(row, rows->end());
    const auto update = map_funding(*row, btc(), k_ingest);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->rate, e9("-0.0000196"));
    EXPECT_EQ(update->interval_s, 8U * 3600U);
    EXPECT_EQ(update->next_funding_time.ns, 1'790'985'600'000'000'000ULL);
    EXPECT_EQ(update->mark_price.raw, 848275);
    EXPECT_EQ(update->accrual, FundingAccrual::Discrete);
}

TEST(BitunixTicker, ReadsBothVolumes) {
    const auto body = fixture("rest_tickers.json");
    const auto storage = std::make_unique<TickerBuffer>();
    const auto rows = parse_tickers(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);
    const auto row = std::ranges::find(*rows, std::string_view{"BTCUSDT"}, &WireTicker::symbol);
    ASSERT_NE(row, rows->end());
    const auto stats = map_ticker_stats(*row, btc(), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("39391.8538"));
    EXPECT_EQ(stats->volume_24h_quote.raw, 3'379'480'939'529'850LL);
}

TEST(BitunixStream, ClassifiesEveryFrame) {
    EXPECT_EQ(parse_head(fixture("ws_connect.json")).channel, Channel::Connect);
    EXPECT_EQ(parse_head(fixture("ws_pong.json")).channel, Channel::Pong);
    EXPECT_EQ(parse_head(fixture("ws_depth_book1.json")).channel, Channel::Book);
    EXPECT_EQ(parse_head(fixture("ws_depth_book15.json")).channel, Channel::Book);
    EXPECT_EQ(parse_head(fixture("ws_trade.json")).channel, Channel::Trade);
    EXPECT_EQ(parse_head(R"({"op":"subscribe","data":{"msg":"subscribe channel > 300","result":false}})").channel,
              Channel::Refused);
    EXPECT_EQ(parse_head("not json").channel, Channel::Unknown);
}

TEST(BitunixStream, TopOfBookCarriesTheExchangeStamp) {
    const auto body = fixture("ws_depth_book1.json");
    LevelBuffer storage;
    const auto wire = parse_book(body, storage);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, btc(), k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 848122);
    EXPECT_EQ(bbo->ask.raw, 848123);
    EXPECT_EQ(bbo->bid_qty.raw, 24888);
    EXPECT_EQ(bbo->ask_qty.raw, 7516);
    EXPECT_EQ(bbo->ts_event.ns, 1'790'964'212'028'000'000ULL);
    EXPECT_EQ(bbo->ts_init, k_ingest);
}

TEST(BitunixStream, DepthIsAWholeLadder) {
    const auto body = fixture("ws_depth_book15.json");
    LevelBuffer storage;
    const auto wire = parse_book(body, storage);
    ASSERT_TRUE(wire.has_value());
    const auto book = map_book(*wire, btc(), k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 15U);
    EXPECT_EQ(book->ask_count, 15U);
    EXPECT_EQ(book->bids[14].price.raw, 848101);
    EXPECT_EQ(book->asks[14].price.raw, 848148);
    EXPECT_EQ(book->ts_event.ns, 1'790'964'212'032'000'000ULL);
}

TEST(BitunixStream, PrintsTakeTheEnvelopeMillisecond) {
    const auto body = fixture("ws_trade.json");
    PrintBuffer storage;
    const auto wire = parse_trades(body, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->prints.size(), 1U);
    const auto trade = map_trade(wire->prints[0], wire->ts_ms, btc(), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 848123);
    EXPECT_EQ(trade->qty.raw, 16);
    EXPECT_EQ(trade->ts_event.ns, 1'790'964'212'024'000'000ULL);
}

TEST(BitunixCaps, ServesPerpAndSpotMarketData) {
    const auto& caps = exec::caps_of(VenueId::Bitunix);
    EXPECT_EQ(caps.venue, VenueId::Bitunix);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_FALSE(caps.fix_order_entry);
}

} // namespace
} // namespace raikiri::venues::bitunix
