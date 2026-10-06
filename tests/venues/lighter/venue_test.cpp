#include <raikiri/domain/asset_class.hpp>
#include <raikiri/venues/lighter/book.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/venues/lighter/map.hpp>
#include <raikiri/venues/lighter/market_data.hpp>
#include <raikiri/venues/lighter/outbox.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::lighter {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'282'300'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

WireMarket market_row(std::uint32_t market_id) {
    static const auto body = fixture("rest_order_book_details.json");
    static MarketBuffer storage;
    const auto rows = parse_order_book_details(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.market_id == market_id) {
            return row;
        }
    }
    ADD_FAILURE() << "no market " << market_id;
    return {};
}

InstrumentBase base_for(std::uint32_t market_id, MarketType market) {
    const auto instrument = map_instrument(market_row(market_id), market);
    EXPECT_TRUE(instrument.has_value());
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Lighter, market, SymbolId{market_id});
    return base;
}

TEST(LighterCatalogue, BothArraysParsePerpsFirst) {
    MarketBuffer storage;
    const auto body = fixture("rest_order_book_details.json");
    const auto rows = parse_order_book_details(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 7U);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ((*rows)[i].market_type, "perp");
    }
    for (std::size_t i = 4; i < 7; ++i) {
        EXPECT_EQ((*rows)[i].market_type, "spot");
        EXPECT_TRUE((*rows)[i].funding_clamp_big.empty()) << (*rows)[i].symbol;
    }
}

TEST(LighterCatalogue, APerpIsNamedByItsUsdcMarginAndSettlesHourly) {
    const auto instrument = map_instrument(market_row(1), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    const auto* perp = std::get_if<PerpInstrument>(&*instrument);
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->base.exchange_symbol.view(), "BTC");
    EXPECT_EQ(perp->base.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(perp->base.tick_size, e9("0.1"));
    EXPECT_EQ(perp->base.lot_size, e9("0.00001"));
    EXPECT_EQ(perp->base.min_notional, e9("10"));
    EXPECT_EQ(perp->base.contract_value, e9("1"));
    EXPECT_EQ(perp->base.kind, ContractKind::Linear);
    EXPECT_EQ(perp->funding_interval_s, 3600U);
    EXPECT_EQ(perp->funding_interval_s, k_funding_interval_s);
}

TEST(LighterCatalogue, TheBigClampIsAnEightHourPercentAndBecomesAnHourlyCap) {
    // "4.0000" is 4% per eight hours, i.e. 0.5% an hour.
    const auto btc = map_instrument(market_row(1), MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_cap, e9("0.005"));
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_floor, e9("-0.005"));
    const auto arc = map_instrument(market_row(132), MarketType::Perp);
    ASSERT_TRUE(arc.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*arc).funding_cap, e9("0.025"));
}

TEST(LighterCatalogue, InactiveRowsAndTheOtherMarketAreRefused) {
    EXPECT_FALSE(map_instrument(market_row(125), MarketType::Perp).has_value());  // DUSK
    EXPECT_FALSE(map_instrument(market_row(2055), MarketType::Spot).has_value()); // AZTEC/USDC
    EXPECT_FALSE(map_instrument(market_row(1), MarketType::Spot).has_value());
    EXPECT_FALSE(map_instrument(market_row(2048), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(market_row(1), MarketType::Future).has_value());
}

TEST(LighterCatalogue, ASpotPairKeepsItsOwnHalves) {
    const auto eth = map_instrument(market_row(2048), MarketType::Spot);
    ASSERT_TRUE(eth.has_value());
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(*eth));
    EXPECT_EQ(base_of(*eth).exchange_symbol.view(), "ETH/USDC");
    EXPECT_EQ(base_of(*eth).local_symbol.view(), "ETH/USDC");
    EXPECT_EQ(base_of(*eth).tick_size, e9("0.01"));
    EXPECT_EQ(base_of(*eth).lot_size, e9("0.0001"));
    const auto spy = map_instrument(market_row(2057), MarketType::Spot);
    ASSERT_TRUE(spy.has_value());
    EXPECT_EQ(base_of(*spy).local_symbol.view(), "RHSPY/USDC");
    EXPECT_EQ(base_of(*spy).contract_value, e9("1"));
}

TEST(LighterCatalogue, TheMarketGroupNamesTheClassAndACoinAmongTheStocksStaysACoin) {
    MarketBuffer storage;
    const auto body = fixture("rest_order_book_details_tradfi.json");
    const auto rows = parse_order_book_details(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 8U);
    const auto key_of = [&](std::string_view symbol) {
        for (const auto& row : *rows) {
            if (row.symbol == symbol) {
                auto instrument = map_instrument(row, MarketType::Perp);
                EXPECT_TRUE(instrument.has_value()) << symbol;
                auto base = base_of(instrument.value_or(Instrument{}));
                classify(VenueId::Lighter, base);
                return asset_key(base);
            }
        }
        ADD_FAILURE() << "no row " << symbol;
        return std::string{};
    };
    EXPECT_EQ(key_of("AAPL"), "equity:AAPL");
    EXPECT_EQ(key_of("SAMSUNGUSD"), "equity:SAMSUNG");
    EXPECT_EQ(key_of("XAU"), "commodity:XAU");
    EXPECT_EQ(key_of("EURUSD"), "fx:EURUSD");
    EXPECT_EQ(key_of("US500"), "index:SPX");
    EXPECT_EQ(key_of("QNT"), "crypto:QNT") << "the coin, filed with the stocks";
    EXPECT_EQ(key_of("OPENAI"), "equity:OPENAI");
    EXPECT_EQ(key_of("STABLECOINX"), "") << "an unnamed group is not guessed";
    EXPECT_EQ(base_of(*map_instrument(market_row(1), MarketType::Perp)).asset_class, AssetClass::Crypto);
    EXPECT_EQ(base_of(*map_instrument(market_row(2048), MarketType::Spot)).asset_class, AssetClass::Crypto);
}

TEST(LighterCatalogue, DecimalsBecomeExactSteps) {
    EXPECT_EQ(step_of(0), e9("1"));
    EXPECT_EQ(step_of(6), e9("0.000001"));
    EXPECT_EQ(step_of(9), FixedE9::from_raw(1));
    EXPECT_FALSE(step_of(10).has_value());
}

TEST(LighterEnvelope, TheTypeSaysWhatAndWhetherItIsTheSnapshot) {
    const auto book = parse_event(fixture("ws_order_book_snapshot.json"));
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->kind, EventKind::OrderBook);
    EXPECT_TRUE(book->snapshot);

    const auto delta = parse_event(fixture("ws_order_book_update.json"));
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->kind, EventKind::OrderBook);
    EXPECT_FALSE(delta->snapshot);

    const auto backlog = parse_event(fixture("ws_trade_snapshot.json"));
    ASSERT_TRUE(backlog.has_value());
    EXPECT_EQ(backlog->kind, EventKind::Trade);
    EXPECT_TRUE(backlog->snapshot);

    const auto stats = parse_event(fixture("ws_market_stats_all.json"));
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->kind, EventKind::MarketStats);

    const auto spot = parse_event(fixture("ws_spot_market_stats.json"));
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->kind, EventKind::SpotMarketStats);
}

TEST(LighterEnvelope, ARefusalHasNoTypeAndNamesNoChannel) {
    const auto rate = fixture("ws_error_rate_limit.json");
    const auto kind = parse_event(rate);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(kind->kind, EventKind::Error);
    const auto err = parse_stream_error(rate);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_rate_limit);

    const auto refusal = fixture("ws_error_too_many_subscriptions.json");
    const auto full = parse_stream_error(refusal);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->code, k_too_many_subscriptions);
}

TEST(LighterEnvelope, TheChannelEndsInTheMarketId) {
    EXPECT_EQ(channel_market("order_book:2054").value_or(0), 2054U);
    EXPECT_EQ(channel_market("trade:1").value_or(0), 1U);
    EXPECT_FALSE(channel_market("market_stats:all").has_value());
    EXPECT_FALSE(channel_market("order_book").has_value());
}

// LDO/USDC, a spot pair: price_decimals 5 and size_decimals 1.
Book ldo_book() {
    return Book{InstrumentId::make(VenueId::Lighter, MarketType::Spot, SymbolId{2054}), e9("0.00001"), e9("0.1")};
}

WireOrderBook parsed(const std::string& text, LevelBuffer& storage) {
    const auto wire = parse_order_book(text, storage);
    EXPECT_TRUE(wire.has_value());
    return *wire;
}

TEST(LighterBook, TheSnapshotIsTheWholeBook) {
    const auto storage = std::make_unique<LevelBuffer>();
    const auto text = fixture("ws_order_book_snapshot.json");
    const auto wire = parsed(text, *storage);
    EXPECT_TRUE(wire.snapshot);
    EXPECT_EQ(wire.market_id, 2054U);
    EXPECT_EQ(wire.begin_nonce, 0U);
    EXPECT_EQ(wire.nonce, 22'900'326'973ULL);
    EXPECT_EQ(wire.bids.size(), 24U);
    EXPECT_EQ(wire.asks.size(), 17U);

    auto book = ldo_book();
    EXPECT_EQ(book.apply(wire), BookResult::Resynced);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(43'804));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(3'425));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(43'888));
    EXPECT_EQ(top->ask_qty, QtyLots::from_raw(6'162));
}

TEST(LighterBook, UpdatesChainOnTheNonceAndAZeroSizeDeletes) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = ldo_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_snapshot.json"), *storage)), BookResult::Resynced);
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_update.json"), *storage)), BookResult::Applied);
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_update_next.json"), *storage)), BookResult::Applied);
    EXPECT_EQ(book.nonce(), 22'900'327'320ULL);
    const auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(43'782));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(3'427));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(43'876));
    EXPECT_EQ(top->ask_qty, QtyLots::from_raw(6'215));
    EXPECT_EQ(book.bids().size(), 24U);
    EXPECT_EQ(book.asks().size(), 18U);
    const auto depth = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->venue_seq, 22'900'327'320ULL);
}

TEST(LighterBook, AMissedFrameDarkensTheBookUntilTheNextSnapshot) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = ldo_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_snapshot.json"), *storage)), BookResult::Resynced);
    EXPECT_EQ(book.apply(parsed(fixture("ws_order_book_update_next.json"), *storage)), BookResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
    EXPECT_EQ(book.apply(parsed(fixture("ws_order_book_update.json"), *storage)), BookResult::Ignored);
    EXPECT_EQ(book.apply(parsed(fixture("ws_order_book_snapshot.json"), *storage)), BookResult::Resynced);
}

TEST(LighterBook, AFrameAlreadyInsideTheBookIsIgnored) {
    const auto storage = std::make_unique<LevelBuffer>();
    auto book = ldo_book();
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_snapshot.json"), *storage)), BookResult::Resynced);
    ASSERT_EQ(book.apply(parsed(fixture("ws_order_book_update.json"), *storage)), BookResult::Applied);
    EXPECT_EQ(book.apply(parsed(fixture("ws_order_book_update.json"), *storage)), BookResult::Ignored);
    EXPECT_TRUE(book.synced());
}

TEST(LighterBook, ALevelOffTheGridDropsTheBookRatherThanRoundingIt) {
    const auto storage = std::make_unique<LevelBuffer>();
    Book coarse{InstrumentId::make(VenueId::Lighter, MarketType::Spot, SymbolId{2054}), e9("0.001"), e9("0.1")};
    EXPECT_EQ(coarse.apply(parsed(fixture("ws_order_book_snapshot.json"), *storage)), BookResult::BadLevel);
    EXPECT_FALSE(coarse.synced());
}

TEST(LighterTrades, OneFrameCarriesEveryPrintOfOneMatch) {
    PrintBuffer storage;
    const auto text = fixture("ws_trade_update.json");
    const auto prints = parse_trades(text, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 3U);
    const auto base = base_for(1, MarketType::Perp);
    const auto trade = map_trade(prints->front(), base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, PriceTicks::from_raw(843'052));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(13));
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'790'282'300'888ULL * 1'000'000ULL);
}

TEST(LighterTrades, TheBacklogReadsBothListsAndIsMarkedAsTheSnapshot) {
    PrintBuffer storage;
    const auto text = fixture("ws_trade_snapshot.json");
    const auto prints = parse_trades(text, storage);
    ASSERT_TRUE(prints.has_value());
    EXPECT_EQ(prints->size(), 2U); // one trade and one liquidation, trimmed
}

TEST(LighterFunding, TheRateIsLightersOwnHourlyPercent) {
    StatsBuffer storage;
    const auto text = fixture("ws_market_stats.json");
    const auto frame = parse_market_stats(text, storage);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->rows.size(), 1U);
    const auto base = base_for(1, MarketType::Perp);
    const auto funding = map_funding(frame->rows[0], base, frame->ts_ms, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // "0.0012" is 0.0012% an hour: 1.2e-5, not 0.0012.
    EXPECT_EQ(funding->rate, e9("0.000012"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(842'896));
    EXPECT_EQ(funding->index_price, PriceTicks::from_raw(843'232));
    EXPECT_EQ(funding->interval_s, 3600U);
    // Paid at 20:00:00.001; the next is the 21:00 mark.
    EXPECT_EQ(funding->next_funding_time.ns, 1'790'283'600'000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, frame->ts_ms * 1'000'000ULL);
}

TEST(LighterFunding, TheAllStreamKeysRowsByMarketAndKeepsTheSign) {
    StatsBuffer storage;
    const auto text = fixture("ws_market_stats_all.json");
    const auto frame = parse_market_stats(text, storage);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->rows.size(), 3U);
    const WireMarketStats* lit = nullptr;
    const WireMarketStats* dusk = nullptr;
    for (const auto& row : frame->rows) {
        if (row.market_id == 120) {
            lit = &row;
        } else if (row.market_id == 125) {
            dusk = &row;
        }
    }
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(dusk, nullptr);
    const auto lit_base = base_for(120, MarketType::Perp);
    const auto negative = map_funding(*lit, lit_base, frame->ts_ms, k_ingest);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative->rate, e9("-0.000036"));

    auto dusk_base = lit_base;
    dusk_base.tick_size = e9("0.00001");
    const auto stale = map_funding(*dusk, dusk_base, frame->ts_ms, k_ingest);
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(stale->next_funding_time.ns, 0U);
}

TEST(LighterFunding, ASpotRowHasTurnoverAndNoRate) {
    StatsBuffer storage;
    const auto text = fixture("ws_spot_market_stats.json");
    const auto frame = parse_market_stats(text, storage);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->rows.size(), 1U);
    const auto base = base_for(2048, MarketType::Spot);
    EXPECT_FALSE(map_funding(frame->rows[0], base, frame->ts_ms, k_ingest).has_value());
    const auto stats = map_ticker_stats(frame->rows[0], base, frame->ts_ms, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("205.6278"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("549839.412448"));
}

TEST(LighterFunding, VolumesAreReadFromTheNumberTextWithoutADouble) {
    StatsBuffer storage;
    const auto text = fixture("ws_market_stats.json");
    const auto frame = parse_market_stats(text, storage);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->rows[0].base_volume, "10699.68227");
    const auto stats = map_ticker_stats(frame->rows[0], base_for(1, MarketType::Perp), frame->ts_ms, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("10699.68227"));
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("899521507.591693"));
    // 171896230.085088 usd at the 84289.6 mark
    EXPECT_EQ(stats->open_interest.raw, 2'039'352'780'000);
}

TEST(LighterWs, TheBookIsAskedForOnceForQuoteAndDepth) {
    std::array<char, 48> bbo_buf{};
    std::array<char, 48> depth_buf{};
    EXPECT_EQ(channel_name(exec::SubKind::Bbo, 1, bbo_buf), "order_book/1");
    EXPECT_EQ(channel_name(exec::SubKind::Depth, 1, depth_buf), "order_book/1");
    std::array<char, 48> trade_buf{};
    EXPECT_EQ(channel_name(exec::SubKind::Trades, 2048, trade_buf), "trade/2048");
    std::array<char, 48> none{};
    EXPECT_TRUE(channel_name(exec::SubKind::Funding, 1, none).empty());
}

TEST(LighterWs, FramesAreTheVenuesOwnShape) {
    Protocol protocol;
    const auto on = protocol.subscribe_frame("order_book/1");
    ASSERT_TRUE(on.has_value());
    EXPECT_EQ(*on, R"({"type":"subscribe","channel":"order_book/1"})");
    const auto off = protocol.unsubscribe_frame(k_perp_stats_all);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, R"({"type":"unsubscribe","channel":"market_stats/all"})");
}

TEST(LighterWs, AReopenAsksForBooksFirstAndEachChannelOnce) {
    const std::vector<std::string> carried{"market_stats/all",
                                           "trade/1",
                                           "order_book/1",
                                           "trade/2",
                                           "spot_market_stats/all",
                                           "order_book/2048",
                                           "order_book/1",
                                           "trade/1"};
    const auto order = ask_order(carried);
    const std::vector<std::string_view> expected{
        "order_book/1", "order_book/2048", "market_stats/all", "spot_market_stats/all", "trade/1", "trade/2"};
    EXPECT_EQ(order, expected);
}

constexpr std::uint64_t k_second = 1'000'000'000ULL;

std::string subscribe(std::string_view channel) {
    Protocol protocol;
    return std::string{*protocol.subscribe_frame(channel)};
}

TEST(LighterOutbox, ARateRefusalAsksForTheOldestUnansweredChannelAgainAndHoldsTheIp) {
    net::IpMeters meters{net::WsLimits{.ip_messages = {{.max = 180, .window_ns = 60 * k_second}}}};
    Outbox outbox{meters};
    const MonoNanos t{9 * k_second};
    ASSERT_TRUE(outbox.admit(subscribe("order_book/1"), t));
    ASSERT_TRUE(outbox.admit(subscribe("market_stats/all"), t));
    ASSERT_TRUE(outbox.admit(subscribe("trade/1"), t));
    ASSERT_TRUE(outbox.admit(R"({"type":"unsubscribe","channel":"order_book/2"})", t));
    EXPECT_EQ(outbox.unanswered(), 3U);

    outbox.answered("order_book:1");
    outbox.answered("market_stats:all");
    EXPECT_EQ(outbox.refused(ep::k_ws_rate_limited, t), "trade/1");
    EXPECT_EQ(outbox.unanswered(), 0U);
    EXPECT_FALSE(meters.take_message(net::steady_ns())) << "a 30009 holds the whole IP";
    EXPECT_EQ(outbox.refused(30003, t), std::nullopt);
}

TEST(LighterEnvelope, TheSnapshotNamesItsChannel) {
    const auto frame = fixture("ws_order_book_snapshot.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_TRUE(event->snapshot);
    EXPECT_EQ(event->channel, "order_book:2054");
}

} // namespace
} // namespace raikiri::venues::lighter
