// Against bytes captured live on 2026-09-25; see fixtures/README.md.

#include <raikiri/venues/edgex/book.hpp>
#include <raikiri/venues/edgex/endpoints.hpp>
#include <raikiri/venues/edgex/map.hpp>
#include <raikiri/venues/edgex/wire.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::edgex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'315'110'500'000'000ULL};

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

WireContract row(std::string_view file, std::uint32_t id) {
    static std::string perp_body;
    static std::string spot_body;
    static ContractBuffer storage;
    const bool spot = file == "rest_meta_spot.json";
    auto& body = spot ? spot_body : perp_body;
    body = fixture(file);
    const auto rows =
        spot ? parse_spot_catalogue(body, storage) : parse_perp_catalogue(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.id == id) {
            return one;
        }
    }
    ADD_FAILURE() << "no contract " << id;
    return {};
}

Instrument perp(std::uint32_t id) {
    const auto instrument = map_instrument(row("rest_meta_perp.json", id), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    return instrument.value_or(Instrument{});
}

Instrument spot(std::uint32_t id) {
    const auto instrument = map_instrument(row("rest_meta_spot.json", id), MarketType::Spot);
    EXPECT_TRUE(instrument.has_value());
    return instrument.value_or(Instrument{});
}

const WireTicker& ticker_of(std::span<const WireTicker> rows, std::uint32_t id) {
    for (const auto& one : rows) {
        if (one.contract_id == id) {
            return one;
        }
    }
    ADD_FAILURE() << "no ticker row " << id;
    static const WireTicker none{};
    return none;
}

TEST(EdgexCatalogue, APerpIsNamedByItsCoinAndSettlesOnItsOwnInterval) {
    const auto btc = perp(30000001);
    const auto* p = std::get_if<PerpInstrument>(&btc);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->base.exchange_symbol.view(), "BTCUSDC");
    EXPECT_EQ(p->base.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(p->base.tick_size, e9("0.1"));
    EXPECT_EQ(p->base.lot_size, e9("0.001"));
    EXPECT_EQ(p->base.contract_value, e9("1"));
    EXPECT_EQ(p->base.kind, ContractKind::Linear);
    EXPECT_EQ(p->funding_interval_s, 14400U);
    EXPECT_EQ(p->funding_cap, e9("0.002"));
    EXPECT_EQ(p->funding_floor, e9("-0.002"));
}

TEST(EdgexCatalogue, TheBaseComesFromTheCoinListNotTheName) {
    const auto hajimi = perp(30000169);
    EXPECT_EQ(base_of(hajimi).exchange_symbol.view(), "哈基米USDC");
    EXPECT_EQ(base_of(hajimi).local_symbol.view(), "HAJIMI/USDC");
    EXPECT_EQ(base_of(hajimi).tick_size, e9("0.00001"));
    EXPECT_EQ(base_of(hajimi).lot_size, e9("1"));
    EXPECT_EQ(std::get<PerpInstrument>(perp(30000174)).funding_cap, e9("0.004"));
}

TEST(EdgexCatalogue, AnIntervalTheRowDoesNotStateIsNotInvented) {
    auto wire = row("rest_meta_perp.json", 30000001);
    wire.funding_interval_min = 0;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 0U);
}

TEST(EdgexCatalogue, ADisabledRowAndOtherMarketsAreRefused) {
    auto wire = row("rest_meta_perp.json", 30000001);
    EXPECT_FALSE(map_instrument(wire, MarketType::Future).has_value());
    wire.enabled = false;
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(EdgexCatalogue, EmptySuccessIsNotACatalogue) {
    ContractBuffer storage;
    const auto empty = parse_perp_catalogue(
        R"({"code":"SUCCESS","data":{"coinList":[],"contractList":[]}})", storage);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    const auto failed = parse_perp_catalogue(R"({"code":"FAILED","data":null})", storage);
    EXPECT_FALSE(failed.has_value());
}

TEST(EdgexCatalogue, SpotPairsKeepTheirSlashedName) {
    ContractBuffer storage;
    const auto body = fixture("rest_meta_spot.json");
    const auto rows = parse_spot_catalogue(body, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 4U);
    const auto eth = spot(90000002);
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(eth));
    EXPECT_EQ(base_of(eth).exchange_symbol.view(), "ETH/USDC");
    EXPECT_EQ(base_of(eth).local_symbol.view(), "ETH/USDC");
    EXPECT_EQ(base_of(eth).tick_size, e9("0.01"));
    EXPECT_EQ(base_of(eth).lot_size, e9("0.000001"));
    EXPECT_EQ(base_of(eth).min_notional, e9("5"));
}

TEST(EdgexEnvelope, TheChannelSaysWhatAndDataTypeWhetherItIsTheSnapshot) {
    const auto snapshot = parse_event(fixture("ws_depth_snapshot.json"));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->kind, EventKind::Depth);
    EXPECT_TRUE(snapshot->snapshot);
    const auto delta = parse_event(fixture("ws_depth_changed.json"));
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->kind, EventKind::Depth);
    EXPECT_FALSE(delta->snapshot);
    const auto backlog = parse_event(fixture("ws_trades_snapshot.json"));
    ASSERT_TRUE(backlog.has_value());
    EXPECT_EQ(backlog->kind, EventKind::Trades);
    EXPECT_TRUE(backlog->snapshot);
    const auto tickers = parse_event(fixture("ws_ticker_all.json"));
    ASSERT_TRUE(tickers.has_value());
    EXPECT_EQ(tickers->kind, EventKind::Ticker);
    const auto ping = parse_event(fixture("ws_ping.json"));
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->kind, EventKind::Ping);
    const auto ack = parse_event(fixture("ws_subscribed.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::Other);
}

TEST(EdgexEnvelope, TheSettledRateChannelIsNotRead) {
    const auto settled = parse_event(fixture("ws_funding_rate_all.json"));
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->kind, EventKind::Other);
}

TEST(EdgexEnvelope, APingIsAnsweredWithItsOwnTime) {
    const auto text = fixture("ws_ping.json");
    const auto time = parse_ping(text);
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, "1790315100007");
    Protocol protocol;
    const auto pong = protocol.pong_frame(*time);
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(*pong, R"({"type":"pong","time":"1790315100007"})");
}

TEST(EdgexEnvelope, ARefusalNamesItsChannelOnlyInsideTheEchoedRequest) {
    const auto contract = fixture("ws_error_contract.json");
    ASSERT_EQ(parse_event(contract).value().kind, EventKind::Error);
    const auto bad_contract = parse_stream_error(contract);
    ASSERT_TRUE(bad_contract.has_value());
    EXPECT_EQ(bad_contract->code, "GATEWAY_INVALID_CONTRACT_ID");
    EXPECT_EQ(bad_contract->channel, "depth.99999999.15");
    const auto depth = fixture("ws_error_depth.json");
    const auto bad_depth = parse_stream_error(depth);
    ASSERT_TRUE(bad_depth.has_value());
    EXPECT_EQ(bad_depth->code, "GATEWAY_INVALID_BOOK_DEPTH");
    EXPECT_EQ(bad_depth->channel, "depth.30000001.50");
    const auto channel = fixture("ws_error_channel.json");
    const auto bad_channel = parse_stream_error(channel);
    ASSERT_TRUE(bad_channel.has_value());
    EXPECT_EQ(bad_channel->channel, "nosuch.30000001");
}

// SOLUSDC: tick 0.01, step 0.1.
Book sol_book(FixedE9 tick = FixedE9::from_raw(10'000'000)) {
    return Book{InstrumentId::make(VenueId::Edgex, MarketType::Perp, SymbolId{3}),
                tick,
                FixedE9::from_raw(100'000'000)};
}

WireDepth parsed(const std::string& text, LevelBuffer& storage) {
    const auto wire = parse_depth(text, storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireDepth{});
}

TEST(EdgexBook, TheSnapshotArrivesOnTheSocket) {
    const auto storage = std::make_unique<LevelBuffer>();
    const auto text = fixture("ws_depth_snapshot.json");
    const auto wire = parsed(text, *storage);
    EXPECT_TRUE(wire.snapshot);
    EXPECT_EQ(wire.contract_id, 30000003U);
    EXPECT_EQ(wire.start_version, 1'146'145'273ULL);
    EXPECT_EQ(wire.end_version, 1'146'145'356ULL);
    EXPECT_EQ(wire.bids.size(), 15U);
    EXPECT_EQ(wire.asks.size(), 15U);
    auto book = sol_book();
    EXPECT_EQ(book.apply(wire), BookResult::Resynced);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(11'644));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(77'294));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(11'650));
    EXPECT_EQ(top->ask_qty, QtyLots::from_raw(68'341));
}

TEST(EdgexBook, DeltasChainOnVersions) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = sol_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_snapshot.json"), *storage)),
              BookResult::Resynced);
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_changed.json"), *storage)), BookResult::Applied);
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_changed_next.json"), *storage)),
              BookResult::Applied);
    EXPECT_EQ(book.version(), 1'146'145'602ULL);
    EXPECT_EQ(book.bids().size(), 15U);
    EXPECT_EQ(book.asks().size(), 15U);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->ask_qty, QtyLots::from_raw(77'254));
    const auto depth = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->venue_seq, 1'146'145'602ULL);
}

TEST(EdgexBook, AMissedDeltaDarkensTheBookUntilTheNextSnapshot) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = sol_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_snapshot.json"), *storage)),
              BookResult::Resynced);
    EXPECT_EQ(book.apply(parsed(fixture("ws_depth_changed_next.json"), *storage)), BookResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
    EXPECT_EQ(book.apply(parsed(fixture("ws_depth_changed.json"), *storage)), BookResult::Ignored);
    EXPECT_EQ(book.apply(parsed(fixture("ws_depth_snapshot.json"), *storage)),
              BookResult::Resynced);
}

TEST(EdgexBook, ADeltaAlreadyInsideTheBookIsIgnored) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = sol_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_snapshot.json"), *storage)),
              BookResult::Resynced);
    ASSERT_EQ(book.apply(parsed(fixture("ws_depth_changed.json"), *storage)), BookResult::Applied);
    EXPECT_EQ(book.apply(parsed(fixture("ws_depth_changed.json"), *storage)), BookResult::Ignored);
    EXPECT_TRUE(book.synced());
}

TEST(EdgexBook, ACrossedTopIsDroppedNotPublished) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = sol_book();
    const auto text = fixture("ws_depth_snapshot.json");
    const auto snapshot = parsed(text, *storage);
    ASSERT_EQ(book.apply(snapshot), BookResult::Resynced);
    const std::array<WireLevel, 1> bid{{{.price = "116.60", .size = "1.0"}}};
    const WireDepth crossing{.contract_id = 30000003,
                             .start_version = snapshot.end_version,
                             .end_version = snapshot.end_version + 1,
                             .bids = bid,
                             .asks = {}};
    EXPECT_EQ(book.apply(crossing), BookResult::Crossed);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
}

TEST(EdgexBook, ALevelOffTheGridDropsTheBook) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto coarse = sol_book(FixedE9::from_raw(100'000'000));
    EXPECT_EQ(coarse.apply(parsed(fixture("ws_depth_snapshot.json"), *storage)),
              BookResult::BadLevel);
    EXPECT_FALSE(coarse.synced());
}

TEST(EdgexBook, SpotSpeaksTheSameProtocol) {
    const auto storage = std::make_unique<LevelBuffer>();
    const auto text = fixture("ws_spot_depth_snapshot.json");
    const auto wire = parsed(text, *storage);
    EXPECT_EQ(wire.contract_id, 90000002U);
    const auto eth = spot(90000002);
    const auto& base = base_of(eth);
    Book book{base.id, base.tick_size, base.lot_size};
    ASSERT_EQ(book.apply(wire), BookResult::Resynced);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(268'029));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(802'670));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(268'036));
}

TEST(EdgexTrades, IsBuyerMakerFalseMeansTheTakerBought) {
    PrintBuffer storage;
    const auto text = fixture("ws_trades_changed.json");
    const auto prints = parse_trades(text, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 2U);
    const auto trade = map_trade(prints->front(), base_of(perp(30000001)), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, PriceTicks::from_raw(842'103));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(21));
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'790'315'110'062ULL * 1'000'000ULL);
}

TEST(EdgexTrades, TheBacklogParsesAndTheSellerSideReads) {
    PrintBuffer storage;
    const auto text = fixture("ws_trades_snapshot.json");
    const auto prints = parse_trades(text, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 2U);
    const auto trade = map_trade((*prints)[1], base_of(perp(30000001)), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(EdgexTrades, ASpotPrintHasNoValueAndStillMaps) {
    PrintBuffer storage;
    const auto text = fixture("ws_spot_trades_changed.json");
    const auto prints = parse_trades(text, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 1U);
    const auto trade = map_trade(prints->front(), base_of(spot(90000002)), k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, PriceTicks::from_raw(268'020));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(12'323));
    EXPECT_EQ(trade->aggressor, Side::Buy);
}

TEST(EdgexFunding, TheTickerRateIsTheForecastNotTheLastSettlement) {
    TickerBuffer storage;
    const auto text = fixture("ws_ticker_all.json");
    const auto rows = parse_tickers(text, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    const auto mrvl = perp(30000077);
    const auto funding = map_funding(ticker_of(*rows, 30000077), mrvl, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // Same second, REST getLatestFundingRate: settled -0.0004477, forecast
    // -0.0004145. The ticker carries the forecast.
    const auto rest = fixture("rest_latest_funding_rate.json");
    EXPECT_NE(rest.find(R"("fundingRate":"-0.0004477","isSettlement":false,)"
                        R"("forecastFundingRate":"-0.0004145")"),
              std::string::npos);
    EXPECT_EQ(funding->rate, e9("-0.0004145"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->interval_s, 14400U);
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'323'200'000ULL * 1'000'000ULL);
    // 263.124307340925419366 on a 0.01 grid.
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(26'312));
}

TEST(EdgexFunding, TheMarkIsTruncatedOntoTheGrid) {
    TickerBuffer storage;
    const auto text = fixture("ws_ticker_all.json");
    const auto rows = parse_tickers(text, storage);
    ASSERT_TRUE(rows.has_value());
    const auto btc = map_funding(ticker_of(*rows, 30000001), perp(30000001), k_ingest);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->rate, e9("0.00005"));
    EXPECT_EQ(btc->mark_price, PriceTicks::from_raw(842'064));
    const auto hajimi = map_funding(ticker_of(*rows, 30000169), perp(30000169), k_ingest);
    ASSERT_TRUE(hajimi.has_value());
    EXPECT_EQ(hajimi->mark_price, PriceTicks::from_raw(3'409));
    EXPECT_TRUE(mark_ticks("0", e9("0.01")).is_undef());
    EXPECT_TRUE(mark_ticks("", e9("0.01")).is_undef());
}

TEST(EdgexFunding, TheTwentyFourHourWindowRidesTheSameRow) {
    TickerBuffer storage;
    const auto text = fixture("ws_ticker_all.json");
    const auto rows = parse_tickers(text, storage);
    ASSERT_TRUE(rows.has_value());
    const auto stats =
        map_ticker_stats(ticker_of(*rows, 30000001), base_of(perp(30000001)), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("4729.559"));
    EXPECT_EQ(stats->volume_24h_quote, e9("397100885.7883"));
}

TEST(EdgexFunding, ASpotRowHasTurnoverAndNoRate) {
    TickerBuffer storage;
    const auto text = fixture("ws_spot_ticker_all.json");
    const auto rows = parse_tickers(text, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);
    const auto eth = spot(90000002);
    const auto& row = ticker_of(*rows, 90000002);
    EXPECT_FALSE(map_funding(row, eth, k_ingest).has_value());
    const auto stats = map_ticker_stats(row, base_of(eth), k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("6727.118297"));
    EXPECT_EQ(stats->volume_24h_quote, e9("17993537.80200688"));
}

TEST(EdgexWs, BboAndDepthShareOneBookKey) {
    std::array<char, 48> bbo{};
    std::array<char, 48> depth{};
    std::array<char, 48> trades{};
    std::array<char, 48> none{};
    EXPECT_EQ(channel_name(exec::SubKind::Bbo, 30000001, bbo), "depth.30000001.15");
    EXPECT_EQ(channel_name(exec::SubKind::Depth, 30000001, depth), "depth.30000001.15");
    EXPECT_EQ(channel_name(exec::SubKind::Trades, 90000002, trades), "trades.90000002");
    EXPECT_TRUE(channel_name(exec::SubKind::Funding, 30000001, none).empty());
    EXPECT_EQ(depth_contract("depth.30000001.15"), 30000001U);
    EXPECT_EQ(depth_contract("trades.30000001"), 0U);
    EXPECT_EQ(depth_contract(k_ticker_all), 0U);
}

TEST(EdgexWs, FramesAreTheVenuesOwnShape) {
    Protocol protocol;
    const auto on = protocol.subscribe_frame("depth.30000001.15");
    ASSERT_TRUE(on.has_value());
    EXPECT_EQ(*on, R"({"type":"subscribe","channel":"depth.30000001.15"})");
    const auto off = protocol.unsubscribe_frame(k_ticker_all);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, R"({"type":"unsubscribe","channel":"ticker.all.1s"})");
}

TEST(EdgexWs, ThePoolStaysUnderWhatOneSocketWasSeenToCarry) {
    EXPECT_LE(k_max_keys_per_connection, ep::k_subscriptions_without_refusal);
    EXPECT_GE(k_max_keys_per_connection * k_max_connections, 2U * 180U + 1U);
}

} // namespace
} // namespace raikiri::venues::edgex
