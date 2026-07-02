// Phemex parser and mapper tests, against bytes captured live on 2026-09-25.
// See fixtures/README.md for provenance.

#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/envelope.hpp>
#include <raikiri/venues/phemex/map.hpp>
#include <raikiri/venues/phemex/market_data.hpp>
#include <raikiri/venues/phemex/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

namespace raikiri::venues::phemex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'315'481'500'000'000ULL};
constexpr std::uint64_t k_hour_ns = 3600ULL * 1'000'000'000ULL;

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

const MappedCatalogue& catalogue(MarketType market) {
    static const auto body = fixture("rest_products.json");
    static const auto storage = std::make_unique<CatalogueBuffer>();
    static const auto wire = parse_products(body, *storage);
    static const auto perp = map_catalogue(*wire, MarketType::Perp);
    static const auto spot = map_catalogue(*wire, MarketType::Spot);
    return market == MarketType::Perp ? perp : spot;
}

const Listed& listed(std::string_view symbol, MarketType market) {
    for (const auto& row : catalogue(market).listed) {
        if (base_of(row.instrument).exchange_symbol.view() == symbol) {
            return row;
        }
    }
    ADD_FAILURE() << "no " << symbol;
    static const Listed none{};
    return none;
}

// --- the catalogue -------------------------------------------------------

TEST(PhemexCatalogue, LoadsOnlyListedRowsOfTheAskedMarket) {
    const auto storage = std::make_unique<CatalogueBuffer>();
    const auto wire = parse_products(fixture("rest_products.json"), *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->perps.size(), 5U);
    EXPECT_EQ(wire->products.size(), 6U);
    EXPECT_EQ(wire->malformed, 0U);
    EXPECT_EQ(catalogue(MarketType::Perp).listed.size(), 6U); // four linear, two inverse
    EXPECT_EQ(catalogue(MarketType::Perp).refused, 0U);
    EXPECT_EQ(catalogue(MarketType::Spot).listed.size(), 2U);
    EXPECT_EQ(catalogue(MarketType::Spot).refused, 0U);
}

TEST(PhemexCatalogue, ALinearPerpReadsDecimalStringsAndItsOwnInterval) {
    const auto& btc = listed("BTCUSDT", MarketType::Perp);
    const auto& perp = std::get<PerpInstrument>(btc.instrument);
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(perp.base.tick_size, e9("0.1"));
    EXPECT_EQ(perp.base.lot_size, e9("0.001"));
    EXPECT_EQ(perp.base.contract_value, e9("1"));
    EXPECT_EQ(perp.base.kind, ContractKind::Linear);
    EXPECT_EQ(perp.funding_interval_s, 28800U);
    EXPECT_TRUE(perp.funding_cap.is_undef());
    EXPECT_EQ(btc.listing.encoding, Encoding::Real);
    // Its description says every 8 hours; the field says four.
    EXPECT_EQ(listed("CAKEUSDT", MarketType::Perp).listing.funding_interval_s, 14400U);
    EXPECT_EQ(listed("XAUUSDT", MarketType::Perp).listing.funding_interval_s, 14400U);
    EXPECT_EQ(base_of(listed("u1000SHIBUSDT", MarketType::Perp).instrument).local_symbol.view(),
              "1000SHIB/USDT");
}

TEST(PhemexCatalogue, ACoinMarginedPerpIsInverseInWholeUsdContracts) {
    const auto& row = listed("BTCUSD", MarketType::Perp);
    const auto& base = base_of(row.instrument);
    EXPECT_EQ(base.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(base.kind, ContractKind::Inverse);
    EXPECT_EQ(base.contract_value, e9("1"));
    EXPECT_EQ(base.tick_size, e9("0.1"));
    EXPECT_EQ(base.lot_size, e9("1"));
    EXPECT_EQ(row.listing.encoding, Encoding::Scaled);
    EXPECT_EQ(row.listing.price_step, 1000); // 0.1 at priceScale 4
    EXPECT_EQ(row.listing.qty_step, 1);
    EXPECT_EQ(row.listing.funding_interval_s, 28800U);
}

TEST(PhemexCatalogue, ASpotPairTakesItsLotFromTheBaseCoinsScale) {
    const auto& btc = listed("sBTCUSDT", MarketType::Spot);
    EXPECT_EQ(base_of(btc.instrument).local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(base_of(btc.instrument).tick_size, e9("0.01"));
    EXPECT_EQ(base_of(btc.instrument).lot_size, e9("0.000001"));
    EXPECT_EQ(base_of(btc.instrument).min_notional, e9("1"));
    EXPECT_EQ(base_of(btc.instrument).taker_fee, e9("0.001"));
    EXPECT_EQ(btc.listing.price_step, 1'000'000);
    EXPECT_EQ(btc.listing.qty_step, 100);
    const auto& shib = listed("sSHIBUSDT", MarketType::Spot);
    EXPECT_NE(shib.listing.volume_scale, 8U);
    EXPECT_EQ(base_of(shib.instrument).lot_size,
              *scaled_e9(shib.listing.qty_step, shib.listing.volume_scale));
}

// --- the two number formats ----------------------------------------------

TEST(PhemexNumbers, TheTwoDecodersRefuseAnythingOffTheGrid) {
    EXPECT_EQ(real_steps("84258.6", e9("0.1")), 842586);
    EXPECT_FALSE(real_steps("84258.65", e9("0.1")).has_value());
    EXPECT_EQ(scaled_steps(8428660000000, 1'000'000), 8428660);
    EXPECT_FALSE(scaled_steps(8428660000001, 1'000'000).has_value());
    EXPECT_EQ(scaled_e9(10000, 8), e9("0.0001"));
    EXPECT_EQ(scaled_e9(842170000, 4), e9("84217"));
    EXPECT_FALSE(scaled_e9(1, 10).has_value());
}

TEST(PhemexNumbers, TheNextSettlementIsTheNextStepOfTheContractsUtcGrid) {
    const UnixNanos now{1'790'315'481'432'695'660ULL}; // 2026-09-25 05:51:21Z
    EXPECT_EQ(next_funding_time(now, 28800).ns, 1'790'323'200ULL * 1'000'000'000ULL); // 08:00
    EXPECT_EQ(next_funding_time(now, 14400).ns, 1'790'323'200ULL * 1'000'000'000ULL); // 08:00
    EXPECT_EQ(next_funding_time(UnixNanos{1'790'323'200ULL * 1'000'000'000ULL}, 14400).ns,
              1'790'323'200ULL * 1'000'000'000ULL + 4 * k_hour_ns);
    EXPECT_EQ(next_funding_time(now, 0).ns, 0U);
}

// --- the book ------------------------------------------------------------

template <typename Level>
std::size_t levels_of(const WireBook<Level>& wire,
                      const Listed& row,
                      std::span<book::Level> bids,
                      std::span<book::Level> asks,
                      std::size_t& ask_count) {
    std::optional<std::size_t> b;
    std::optional<std::size_t> a;
    if constexpr (std::is_same_v<Level, WireRealLevel>) {
        b = to_levels(wire.bids, base_of(row.instrument), bids);
        a = to_levels(wire.asks, base_of(row.instrument), asks);
    } else {
        b = to_levels(wire.bids, row.listing, bids);
        a = to_levels(wire.asks, row.listing, asks);
    }
    EXPECT_TRUE(b.has_value() && a.has_value());
    ask_count = a.value_or(0);
    return b.value_or(0);
}

template <typename Level>
BookResult feed(Book& book, const std::string& frame, const Listed& row) {
    static LevelBuffer<Level> storage;
    std::expected<WireBook<Level>, WireError> wire;
    if constexpr (std::is_same_v<Level, WireRealLevel>) {
        wire = parse_real_book(frame, storage);
    } else {
        wire = parse_scaled_book(frame, storage);
    }
    EXPECT_TRUE(wire.has_value());
    std::array<book::Level, k_max_levels> bids{};
    std::array<book::Level, k_max_levels> asks{};
    std::size_t ask_count = 0;
    const auto bid_count = levels_of(*wire, row, bids, asks, ask_count);
    return book.apply(wire->head.snapshot,
                      std::span{bids.data(), bid_count},
                      std::span{asks.data(), ask_count},
                      wire->head.depth,
                      wire->head.sequence);
}

TEST(PhemexBook, APerpBookIsReplacedByEverySnapshotAndCutToItsDepth) {
    const auto& row = listed("BTCUSDT", MarketType::Perp);
    Book book{base_of(row.instrument).id};
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_update.json"), row),
              BookResult::Ignored);
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_snapshot.json"), row),
              BookResult::Replaced);
    EXPECT_EQ(book.bids().size(), 30U);
    EXPECT_EQ(book.asks().size(), 30U);
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_update.json"), row),
              BookResult::Applied);
    EXPECT_LE(book.asks().size(), 30U);
    const auto has_ask = [&](std::int64_t raw) {
        return std::ranges::any_of(book.asks(), [&](const auto& l) { return l.price.raw == raw; });
    };
    EXPECT_FALSE(has_ask(842330)); // "84233","0" deleted it
    EXPECT_TRUE(has_ask(842346));
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_resnapshot.json"), row),
              BookResult::Replaced);
    EXPECT_EQ(book.bids().size(), 30U);
    EXPECT_EQ(book.replacements(), 2U);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_LT(top->bid.raw, top->ask.raw);
}

TEST(PhemexBook, ASpotBookDecodesScaledIntegers) {
    const auto& row = listed("sBTCUSDT", MarketType::Spot);
    Book book{base_of(row.instrument).id};
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_spot_book_snapshot.json"), row),
              BookResult::Replaced);
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_spot_book_update.json"), row),
              BookResult::Applied);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_GT(top->bid.raw, 8'000'000); // above 80 000.00 in 0.01 ticks
    EXPECT_LT(top->ask.raw, 9'000'000);
}

TEST(PhemexBook, AnInverseBookCountsWholeContracts) {
    const auto& row = listed("BTCUSD", MarketType::Perp);
    Book book{base_of(row.instrument).id};
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_inverse_book_snapshot.json"), row),
              BookResult::Replaced);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_GT(top->bid.raw, 800'000); // above 80 000.0 in 0.1 ticks
}

TEST(PhemexBook, ACrossedTopDropsTheBookUntilTheNextSnapshot) {
    Book book{InstrumentId{}};
    const std::array<book::Level, 1> bids{{{PriceTicks::from_raw(100), QtyLots::from_raw(1)}}};
    const std::array<book::Level, 1> asks{{{PriceTicks::from_raw(101), QtyLots::from_raw(1)}}};
    const std::array<book::Level, 1> through{{{PriceTicks::from_raw(102), QtyLots::from_raw(1)}}};
    EXPECT_EQ(book.apply(true, bids, asks, 30, 1), BookResult::Replaced);
    EXPECT_EQ(book.apply(false, through, {}, 30, 2), BookResult::Crossed);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.apply(false, bids, {}, 30, 3), BookResult::Ignored);
    EXPECT_EQ(book.apply(true, bids, asks, 30, 4), BookResult::Replaced);
    EXPECT_EQ(book.crossings(), 1U);
}

// --- trades --------------------------------------------------------------

TEST(PhemexTrades, TheSubscribeBacklogIsFlaggedAndTheTapeCarriesTheTakerSide) {
    PrintBuffer<WireRealPrint> storage;
    const auto backlog = parse_real_trades(fixture("ws_real_trades_snapshot.json"), storage);
    ASSERT_TRUE(backlog.has_value());
    EXPECT_TRUE(backlog->snapshot);
    const auto tape_text = fixture("ws_real_trades.json");
    const auto tape = parse_real_trades(tape_text, storage);
    ASSERT_TRUE(tape.has_value());
    EXPECT_FALSE(tape->snapshot);
    ASSERT_FALSE(tape->prints.empty());
    const auto& base = base_of(listed("BTCUSDT", MarketType::Perp).instrument);
    const auto trade = map_trade(tape->prints[0], base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, tape->prints[0].taker_bought ? Side::Buy : Side::Sell);
    EXPECT_EQ(trade->ts_event.ns, tape->prints[0].ts_ns);
}

TEST(PhemexTrades, ScaledPrintsDecodeOnTheirOwnGrid) {
    PrintBuffer<WireScaledPrint> storage;
    for (const auto& [file, symbol, market] :
         {std::tuple{"ws_spot_trades.json", "sBTCUSDT", MarketType::Spot},
          std::tuple{"ws_inverse_trades.json", "BTCUSD", MarketType::Perp}}) {
        const auto text = fixture(file);
        const auto tape = parse_scaled_trades(text, storage);
        ASSERT_TRUE(tape.has_value()) << file;
        EXPECT_EQ(tape->symbol, symbol);
        const auto& row = listed(symbol, market);
        for (const auto& print : tape->prints) {
            EXPECT_TRUE(map_trade(print, base_of(row.instrument), row.listing, k_ingest).has_value())
                << file;
        }
    }
}

// --- funding and the 24-hour window --------------------------------------

TEST(PhemexFunding, ThePackNamesItsColumnsOnceAndTheUpdatesReuseThem) {
    PackColumns columns;
    PackBuffer storage;
    EXPECT_FALSE(parse_perp_pack(fixture("ws_perp_pack_update.json"), columns, storage).has_value());
    const auto snapshot = parse_perp_pack(fixture("ws_perp_pack_snapshot.json"), columns, storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->rows.size(), 5U);
    EXPECT_TRUE(parse_perp_pack(fixture("ws_perp_pack_update.json"), columns, storage).has_value());
}

TEST(PhemexFunding, TheRateIsPerTheContractsOwnIntervalAndTheNextTimeIsOnItsGrid) {
    PackColumns columns;
    PackBuffer storage;
    const auto text = fixture("ws_perp_pack_snapshot.json");
    const auto pack = parse_perp_pack(text, columns, storage);
    ASSERT_TRUE(pack.has_value());
    const auto row_of = [&](std::string_view symbol) {
        for (const auto& row : pack->rows) {
            if (row.symbol == symbol) {
                return row;
            }
        }
        return WirePackRow{};
    };
    const auto& cake = listed("CAKEUSDT", MarketType::Perp);
    const auto funding =
        map_funding(row_of("CAKEUSDT"), base_of(cake.instrument), cake.listing, pack->ts_ns, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00005"));
    EXPECT_EQ(funding->predicted_rate, e9("0.00005"));
    EXPECT_EQ(funding->interval_s, 14400U);
    EXPECT_EQ(funding->next_funding_time.ns % (4 * k_hour_ns), 0U);
    EXPECT_GT(funding->next_funding_time.ns, pack->ts_ns);
    EXPECT_LE(funding->next_funding_time.ns - pack->ts_ns, 4 * k_hour_ns);
    EXPECT_EQ(funding->mark_price.raw, 27408); // markRp, not the indexRp beside it

    const auto& btc = listed("BTCUSDT", MarketType::Perp);
    const auto btc_funding =
        map_funding(row_of("BTCUSDT"), base_of(btc.instrument), btc.listing, pack->ts_ns, k_ingest);
    ASSERT_TRUE(btc_funding.has_value());
    EXPECT_EQ(btc_funding->interval_s, 28800U);
    EXPECT_EQ(btc_funding->next_funding_time.ns % (8 * k_hour_ns), 0U);
    const auto stats = map_ticker_stats(row_of("BTCUSDT"), base_of(btc.instrument), pack->ts_ns, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("4551.127"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("382619179.95088"));

    EXPECT_FALSE(map_funding(WirePackRow{}, base_of(btc.instrument), btc.listing, pack->ts_ns, k_ingest)
                     .has_value());
}

TEST(PhemexFunding, TheInverseTickerIsScaledAndCountsContractsAsTheQuote) {
    const auto text = fixture("ws_inverse_ticker.json");
    const auto wire = parse_inverse_ticker(text);
    ASSERT_TRUE(wire.has_value());
    const auto& row = listed("BTCUSD", MarketType::Perp);
    const auto funding = map_funding(*wire, base_of(row.instrument), row.listing, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.0001"));
    EXPECT_EQ(funding->interval_s, 28800U);
    EXPECT_EQ(funding->mark_price.raw, 842170); // 84217.0 in 0.1 ticks
    const auto stats = map_ticker_stats(*wire, base_of(row.instrument), row.listing, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("195087675"));
    EXPECT_EQ(stats->volume_24h_base, e9("2329.70059866"));
}

TEST(PhemexFunding, TheSpotTickerStatesBothVolumes) {
    const auto text = fixture("ws_spot_ticker.json");
    const auto wire = parse_spot_ticker(text);
    ASSERT_TRUE(wire.has_value());
    const auto& row = listed("sBTCUSDT", MarketType::Spot);
    const auto stats = map_ticker_stats(*wire, base_of(row.instrument), row.listing, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, *scaled_e9(*wire->volume_ev, 8));
    EXPECT_EQ(stats->volume_24h_quote, FixedE6::from_raw(*wire->turnover_ev / 100));
}

// --- the socket ----------------------------------------------------------

TEST(PhemexEnvelope, EveryFrameIsRecognisedByItsPayloadKey) {
    const std::array<std::pair<std::string_view, EventKind>, 12> cases{{
        {"ws_real_book_snapshot.json", EventKind::RealBook},
        {"ws_spot_book_update.json", EventKind::ScaledBook},
        {"ws_inverse_book_snapshot.json", EventKind::ScaledBook},
        {"ws_real_trades.json", EventKind::RealTrades},
        {"ws_spot_trades.json", EventKind::ScaledTrades},
        {"ws_perp_pack_snapshot.json", EventKind::PerpPack},
        {"ws_perp_pack_update.json", EventKind::PerpPack},
        {"ws_inverse_ticker.json", EventKind::InverseTicker},
        {"ws_spot_ticker.json", EventKind::SpotTicker},
        {"ws_ack.json", EventKind::Reply},
        {"ws_pong.json", EventKind::Reply},
        {"ws_error_subscription_limit.json", EventKind::Error},
    }};
    for (const auto& [file, kind] : cases) {
        const auto event = parse_event(fixture(file));
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(*event, kind) << file;
    }
    const auto error = parse_stream_error(fixture("ws_error_subscription_limit.json"));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, k_ws_subscription_limit);
    EXPECT_EQ(error->id, 102U);
}

TEST(PhemexWs, KeysAndFramesFollowTheInstrumentsEncoding) {
    std::array<char, 64> buf{};
    EXPECT_EQ(channel_key(exec::SubKind::Bbo, MarketType::Perp, Encoding::Real, "BTCUSDT", buf),
              "orderbook_p:BTCUSDT");
    EXPECT_EQ(channel_key(exec::SubKind::Trades, MarketType::Perp, Encoding::Scaled, "BTCUSD", buf),
              "trade:BTCUSD");
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Perp, Encoding::Real, "BTCUSDT", buf),
              k_perp_pack);
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Perp, Encoding::Scaled, "BTCUSD", buf),
              k_inverse_ticker);
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Spot, Encoding::Scaled, "sBTCUSDT", buf),
              k_spot_ticker);
    EXPECT_TRUE(is_book_key("orderbook:sBTCUSDT"));
    EXPECT_EQ(key_symbol("orderbook:sBTCUSDT"), "sBTCUSDT");
    Protocol protocol;
    EXPECT_EQ(protocol.subscribe_frame("orderbook_p:BTCUSDT"),
              R"({"id":1,"method":"orderbook_p.subscribe","params":["BTCUSDT"]})");
    EXPECT_EQ(protocol.subscribe_frame("perp_market24h_pack_p"),
              R"({"id":2,"method":"perp_market24h_pack_p.subscribe","params":[]})");
}

} // namespace
} // namespace raikiri::venues::phemex
