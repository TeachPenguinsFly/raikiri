#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/catalogue.hpp>
#include <raikiri/venues/phemex/envelope.hpp>
#include <raikiri/venues/phemex/map.hpp>
#include <raikiri/venues/phemex/market_data.hpp>
#include <raikiri/venues/phemex/trade.hpp>
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
    // the description says 8 hours; the field says four
    EXPECT_EQ(listed("CAKEUSDT", MarketType::Perp).listing.funding_interval_s, 14400U);
    EXPECT_EQ(listed("XAUUSDT", MarketType::Perp).listing.funding_interval_s, 14400U);
    EXPECT_EQ(base_of(listed("u1000SHIBUSDT", MarketType::Perp).instrument).local_symbol.view(), "1000SHIB/USDT");
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
    EXPECT_EQ(base_of(shib.instrument).lot_size, *scaled_e9(shib.listing.qty_step, shib.listing.volume_scale));
}

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
    const UnixNanos now{1'790'315'481'432'695'660ULL};                                // 2026-09-25 05:51:21Z
    EXPECT_EQ(next_funding_time(now, 28800).ns, 1'790'323'200ULL * 1'000'000'000ULL); // 08:00
    EXPECT_EQ(next_funding_time(now, 14400).ns, 1'790'323'200ULL * 1'000'000'000ULL); // 08:00
    EXPECT_EQ(next_funding_time(UnixNanos{1'790'323'200ULL * 1'000'000'000ULL}, 14400).ns,
              1'790'323'200ULL * 1'000'000'000ULL + 4 * k_hour_ns);
    EXPECT_EQ(next_funding_time(now, 0).ns, 0U);
}

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

template <typename Level> BookResult feed(Book& book, const std::string& frame, const Listed& row) {
    static LevelBuffer<Level> storage;
    const auto wire = [&] {
        if constexpr (std::is_same_v<Level, WireRealLevel>) {
            return parse_real_book(frame, storage);
        } else {
            return parse_scaled_book(frame, storage);
        }
    }();
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
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_update.json"), row), BookResult::Ignored);
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_snapshot.json"), row), BookResult::Replaced);
    EXPECT_EQ(book.bids().size(), 30U);
    EXPECT_EQ(book.asks().size(), 30U);
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_update.json"), row), BookResult::Applied);
    EXPECT_LE(book.asks().size(), 30U);
    const auto has_ask = [&](std::int64_t raw) {
        return std::ranges::any_of(book.asks(), [&](const auto& l) { return l.price.raw == raw; });
    };
    EXPECT_FALSE(has_ask(842330)); // "84233","0" deleted it
    EXPECT_TRUE(has_ask(842346));
    EXPECT_EQ(feed<WireRealLevel>(book, fixture("ws_real_book_resnapshot.json"), row), BookResult::Replaced);
    EXPECT_EQ(book.bids().size(), 30U);
    EXPECT_EQ(book.replacements(), 2U);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_LT(top->bid.raw, top->ask.raw);
}

TEST(PhemexBook, ASpotBookDecodesScaledIntegers) {
    const auto& row = listed("sBTCUSDT", MarketType::Spot);
    Book book{base_of(row.instrument).id};
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_spot_book_snapshot.json"), row), BookResult::Replaced);
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_spot_book_update.json"), row), BookResult::Applied);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_GT(top->bid.raw, 8'000'000); // above 80 000.00 in 0.01 ticks
    EXPECT_LT(top->ask.raw, 9'000'000);
}

TEST(PhemexBook, AnInverseBookCountsWholeContracts) {
    const auto& row = listed("BTCUSD", MarketType::Perp);
    Book book{base_of(row.instrument).id};
    EXPECT_EQ(feed<WireScaledLevel>(book, fixture("ws_inverse_book_snapshot.json"), row), BookResult::Replaced);
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
    for (const auto& [file, symbol, market] : {std::tuple{"ws_spot_trades.json", "sBTCUSDT", MarketType::Spot},
                                               std::tuple{"ws_inverse_trades.json", "BTCUSD", MarketType::Perp}}) {
        const auto text = fixture(file);
        const auto tape = parse_scaled_trades(text, storage);
        ASSERT_TRUE(tape.has_value()) << file;
        EXPECT_EQ(tape->symbol, symbol);
        const auto& row = listed(symbol, market);
        for (const auto& print : tape->prints) {
            EXPECT_TRUE(map_trade(print, base_of(row.instrument), row.listing, k_ingest).has_value()) << file;
        }
    }
}

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
    const auto funding = map_funding(row_of("CAKEUSDT"), base_of(cake.instrument), cake.listing, pack->ts_ns, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00005"));
    EXPECT_EQ(funding->predicted_rate, e9("0.00005"));
    EXPECT_EQ(funding->interval_s, 14400U);
    EXPECT_EQ(funding->next_funding_time.ns % (4 * k_hour_ns), 0U);
    EXPECT_GT(funding->next_funding_time.ns, pack->ts_ns);
    EXPECT_LE(funding->next_funding_time.ns - pack->ts_ns, 4 * k_hour_ns);
    EXPECT_EQ(funding->mark_price.raw, 27408);  // markRp, not the indexRp beside it
    EXPECT_EQ(funding->index_price.raw, 27450); // 2.745025 truncated onto the tick

    const auto& btc = listed("BTCUSDT", MarketType::Perp);
    const auto btc_funding = map_funding(
        row_of("BTCUSDT"), base_of(btc.instrument), btc.listing, pack->ts_ns, k_ingest);
    ASSERT_TRUE(btc_funding.has_value());
    EXPECT_EQ(btc_funding->interval_s, 28800U);
    EXPECT_EQ(btc_funding->next_funding_time.ns % (8 * k_hour_ns), 0U);
    const auto stats = map_ticker_stats(row_of("BTCUSDT"), base_of(btc.instrument), pack->ts_ns, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("4551.127"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("382619179.95088"));
    EXPECT_EQ(stats->open_interest, e9("1942.9502556")); // openInterestRv is coins

    EXPECT_FALSE(map_funding(WirePackRow{}, base_of(btc.instrument), btc.listing, pack->ts_ns, k_ingest).has_value());
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
    EXPECT_EQ(funding->index_price.raw, 842446);
    const auto stats = map_ticker_stats(*wire, base_of(row.instrument), row.listing, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("195087675"));
    EXPECT_EQ(stats->volume_24h_base, e9("2329.70059866"));
    // 942594395 one-dollar contracts at the 84217.0 mark
    EXPECT_EQ(stats->open_interest.raw, 11'192'448'021'183);
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

TEST(PhemexFunding, TheSpotTableStatesEveryPairsVolumesInOneCall) {
    const auto text = fixture("rest_spot_ticker_24h_all.json");
    static SpotTickerBuffer storage;
    const auto rows = parse_spot_tickers(text, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    const auto btc = std::ranges::find(*rows, std::string_view{"sBTCUSDT"}, &WireSpotTicker::symbol);
    ASSERT_NE(btc, rows->end());
    EXPECT_EQ(btc->volume_ev, 360'009'315'300);
    EXPECT_EQ(btc->turnover_ev, 30'880'846'524'733'312);
    EXPECT_EQ(btc->ts_ns, 1'791'299'604'634'776'014ULL);

    const auto& row = listed("sBTCUSDT", MarketType::Spot);
    const auto stats = map_ticker_stats(*btc, base_of(row.instrument), row.listing, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("3600.093153"));
    EXPECT_EQ(stats->volume_24h_quote, FixedE6::from_raw(308'808'465'247'333));
    EXPECT_TRUE(stats->open_interest.is_undef());
}

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
    EXPECT_EQ(channel_key(exec::SubKind::Bbo, MarketType::Perp, Encoding::Real, "BTCUSDT", buf), "orderbook_p:BTCUSDT");
    EXPECT_EQ(channel_key(exec::SubKind::Trades, MarketType::Perp, Encoding::Scaled, "BTCUSD", buf), "trade:BTCUSD");
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Perp, Encoding::Real, "BTCUSDT", buf), k_perp_pack);
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Perp, Encoding::Scaled, "BTCUSD", buf), k_inverse_ticker);
    EXPECT_EQ(channel_key(exec::SubKind::Funding, MarketType::Spot, Encoding::Scaled, "sBTCUSDT", buf), k_spot_ticker);
    EXPECT_TRUE(is_book_key("orderbook:sBTCUSDT"));
    EXPECT_EQ(key_symbol("orderbook:sBTCUSDT"), "sBTCUSDT");
    Protocol protocol;
    EXPECT_EQ(protocol.subscribe_frame("orderbook_p:BTCUSDT"),
              R"({"id":1,"method":"orderbook_p.subscribe","params":["BTCUSDT"]})");
    EXPECT_EQ(protocol.subscribe_frame("perp_market24h_pack_p"),
              R"({"id":2,"method":"perp_market24h_pack_p.subscribe","params":[]})");
}

exec::InstrumentTable btc_table() {
    exec::InstrumentTable table;
    EXPECT_TRUE(table.add(listed("BTCUSDT", MarketType::Perp).instrument).has_value());
    return table;
}

ExchangeClientOrderId coid() {
    return *ExchangeClientOrderId::from("01HZY3M7K8Q9R0S1T2V3W4X5Y6");
}

constexpr std::string_view k_doc_uuid = "c0327a7d-9064-62a9-28f6-2db9aaaa04e0";

// the docs print the signed strings but no secret; the digests are python hmac's over the same bytes
TEST(PhemexSigner, SignsPathQueryExpiryAndBodyAsTheDocsJoinThem) {
    net::Signer signer;
    signer.set_api_key("806066b0-f02b-4d3e-b444-76ec718e1023");
    ASSERT_TRUE(signer.secret().assign("not-a-real-secret-0123456789abcdef").has_value());
    std::array<char, net::k_hex_bytes + 1> out{};
    EXPECT_EQ(sign_request(signer, "/accounts/accountPositions", "currency=BTC", 1575735514, {}, out),
              "4393fea89386c1ab329a183b781368bf0fafd2d7464e9a80829956bac37db339");
    EXPECT_EQ(sign_request(signer,
                           "/orders",
                           {},
                           1575735514,
                           R"({"symbol":"BTCUSD","clOrdID":"uuid-1573058952273","side":"Sell","priceEp":93185000,)"
                           R"("orderQty":7,"ordType":"Limit","reduceOnly":false,"timeInForce":"GoodTillCancel",)"
                           R"("takeProfitEp":0,"stopLossEp":0})",
                           out),
              "6b62de98aff0038c4876684cd2406ee37bb1d3f4be49589290162b1565fdc96f");
    EXPECT_EQ(sign_request(signer, "/g-orders/activeList", "symbol=BTCUSDT", 1575735514, {}, out),
              "7560e7573ec14cf3e7855e8142826e32c188a1bd411cda52b1925737c3669d0a");

    // user.auth signs the key and the expiry, in seconds
    const auto auth = auth_frame(signer, UnixNanos{1'570'091'172'000'000'000ULL}, 7);
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth,
              R"({"method":"user.auth","params":["API","806066b0-f02b-4d3e-b444-76ec718e1023",)"
              R"("65d9a9434b7918a95deba4569a445a6804d4663248b22c3e9909d63259feea5d",1570091232],"id":7})");
}

TEST(PhemexRequests, SignsTheQueryItSendsAndRefusesWithoutKeys) {
    const auto& base = base_of(listed("BTCUSDT", MarketType::Perp).instrument);
    net::Signer signer;
    RequestBuilder unkeyed{signer};
    OrderCmd cmd{};
    cmd.instrument = base.id;
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    cmd.qty = QtyLots::from_raw(5);
    cmd.price = PriceTicks::from_raw(848001);
    cmd.exchange_client_order_id = coid();
    const UnixNanos now{1'575'735'454'000'000'000ULL};
    EXPECT_EQ(unkeyed.create_order(cmd, base, now).error(), BuildError::NoCredentials);

    signer.set_api_key("key");
    ASSERT_TRUE(signer.secret().assign("secret").has_value());
    RequestBuilder builder{signer};
    const auto request = builder.create_order(cmd, base, now);
    ASSERT_TRUE(request.has_value());
    const std::string query{"clOrdID=01HZY3M7K8Q9R0S1T2V3W4X5Y6&symbol=BTCUSDT&side=Sell&posSide=Merged&ordType=Limit"
                            "&orderQtyRq=0.005&priceRp=84800.1&timeInForce=PostOnly&reduceOnly=true"};
    EXPECT_EQ(builder.last_signed(), "/g-orders/create" + query + "1575735514");
    EXPECT_EQ(request->target, "/g-orders/create?" + query);
    EXPECT_EQ(request->method, net::HttpMethod::Put);
    ASSERT_EQ(request->headers.size(), 3U);
    EXPECT_EQ(request->headers[1].value, "1575735514");
    std::array<char, net::k_hex_bytes + 1> out{};
    EXPECT_EQ(request->headers[2].value, sign_request(signer, "/g-orders/create", query, 1575735514, {}, out));

    const auto cancel = builder.cancel_order(cmd, "BTCUSDT", now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->method, net::HttpMethod::Delete);
    EXPECT_EQ(cancel->target, "/g-orders/cancel?clOrdID=01HZY3M7K8Q9R0S1T2V3W4X5Y6&posSide=Merged&symbol=BTCUSDT");

    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Fok;
    ASSERT_TRUE(builder.create_order(cmd, base, now).has_value());
    EXPECT_NE(builder.last_signed().find("timeInForce=FillOrKill"), std::string_view::npos);
    const auto amend = builder.amend_order(cmd, base, now);
    ASSERT_TRUE(amend.has_value());
    EXPECT_EQ(amend->target,
              "/g-orders/replace?symbol=BTCUSDT&origClOrdID=01HZY3M7K8Q9R0S1T2V3W4X5Y6&priceRp=84800.1"
              "&orderQtyRq=0.005&posSide=Merged");
}

TEST(PhemexErrors, ReadsStringAndNumberCodes) {
    const auto bad_key = parse_reply(fixture("rest_error_bad_key.json"));
    ASSERT_TRUE(bad_key.has_value());
    EXPECT_EQ(bad_key->code, k_unauthorized);
    EXPECT_EQ(classify(bad_key->code, 401), exec::ErrorClass::AuthFailure);
    const auto ok = parse_reply(fixture("rest_create_order.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);
    EXPECT_EQ(classify(k_no_balance, 200), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_too_many_requests, 200), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(0, 503), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
}

TEST(PhemexUserStream, AuthRepliesAndOrderRowsMapToLifecycleAndFills) {
    const auto table = btc_table();
    const auto storage = std::make_unique<OrderRowBuffer>();
    const auto ok = parse_private(fixture("ws_auth_ok.json"), *storage);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->kind, PrivateKind::Reply);
    EXPECT_EQ(ok->error_code, 0);
    const auto refused = parse_private(fixture("ws_auth_refused.json"), *storage);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->error_code, k_invalid_login_token);

    const auto snapshot = parse_private(fixture("ws_aop_p_snapshot.json"), *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->kind, PrivateKind::Snapshot);

    // the doc's uuid client id is longer than the ulids this oms mints, so its rows are re-keyed
    auto push_body = fixture("ws_aop_p_incremental.json");
    for (auto at = push_body.find(k_doc_uuid); at != std::string::npos; at = push_body.find(k_doc_uuid)) {
        push_body.replace(at, k_doc_uuid.size(), coid().view());
    }
    const auto push = parse_private(push_body, *storage);
    ASSERT_TRUE(push.has_value());
    EXPECT_EQ(push->kind, PrivateKind::Orders);
    ASSERT_EQ(push->orders.size(), 2U);
    const auto accepted = map_order_row(push->orders[0], table, k_ingest);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->qty.raw, 3);
    EXPECT_EQ(accepted->exchange_order_id.view(), "fa64c6f2-47a4-4929-aab4-b7fa9bbc4323");
    const auto fill = map_order_row(push->orders[1], table, k_ingest);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->qty.raw, 3);
    EXPECT_EQ(fill->leaves_qty.raw, 0);
    EXPECT_EQ(fill->price.raw, 207251);
    EXPECT_EQ(fill->fee, e9("0.03730518"));
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->exec_id.view(), "b6c8d16b-c777-510c-8476-80f399b2d5ad");
    EXPECT_EQ(fill->ts_event.ns, 1'666'858'780'881'545'305ULL);
}

TEST(PhemexReconciliation, ReadsOrdersFillsPositionsAndTheUsdtAccount) {
    const auto table = btc_table();
    const auto storage = std::make_unique<OrderRowBuffer>();
    const auto active_body = fixture("rest_active_list.json");
    const auto active = parse_order_rows(active_body, *storage);
    ASSERT_TRUE(active.has_value());
    ASSERT_FALSE(active->empty());
    const auto report = map_order_report(active->front(), table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->qty.raw, 5);
    EXPECT_EQ(report->leaves.raw, 5);
    EXPECT_EQ(report->price.raw, 224904);
    EXPECT_EQ(report->side, Side::Sell);

    auto by_id_body = fixture("rest_order_by_id.json");
    by_id_body.replace(by_id_body.find("cfffa744-712d-867a-e397-9888eec3f6d1"), 36, coid().view());
    const auto by_id = parse_order_rows(by_id_body, *storage);
    ASSERT_TRUE(by_id.has_value());
    ASSERT_EQ(by_id->size(), 1U);
    EXPECT_EQ(by_id->front().order_id, "743fc923-cb01-4261-88d1-b35dba2cdac0");
    EXPECT_EQ(map_order_report(by_id->front(), table)->status, OrderStatus::Filled);

    const auto trades_body = fixture("rest_user_trades.json");
    const auto trades = parse_order_rows(trades_body, *storage);
    ASSERT_TRUE(trades.has_value());
    const auto fill = map_fill_report(trades->front(), table);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->qty.raw, 1);
    EXPECT_EQ(fill->price.raw, 166000);
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->fee, e9("0.00166"));

    const auto positions_storage = std::make_unique<PositionBuffer>();
    const auto account_body = fixture("rest_account_positions.json");
    const auto account = parse_account_positions(account_body, *positions_storage);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->positions.size(), 2U);
    const auto balance = map_balance(account->account, k_ingest);
    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ(balance->total, e9("730.97309163"));
    EXPECT_EQ(balance->available, e9("729.95271609"));

    const auto short_btc = map_position(
        WirePosition{.symbol = "BTCUSDT", .side = "Sell", .size = "0.007", .avg_entry_price = "84000"}, table);
    ASSERT_TRUE(short_btc.has_value());
    EXPECT_EQ(short_btc->net_qty.raw, -7);
    EXPECT_EQ(short_btc->avg_entry.raw, 840000);
}

TEST(PhemexCaps, StatesRestOrderEntryOnUsdtPerps) {
    const auto& caps = exec::caps_of(VenueId::Phemex);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_TRUE(caps.cancel_by_client_order_id);
    EXPECT_TRUE(caps.amend_order);
    EXPECT_TRUE(caps.native_post_only);
    EXPECT_TRUE(caps.position_pull.perp);
    EXPECT_FALSE(caps.order_pull.spot);
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::AuthSubscribe);
}

} // namespace
} // namespace raikiri::venues::phemex
