#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/arcus/book.hpp>
#include <raikiri/venues/arcus/map.hpp>
#include <raikiri/venues/arcus/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::arcus {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'791'299'100'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    return parse_decimal_e9(text).value_or(FixedE9{});
}

WireMarket row(std::string_view symbol) {
    static const auto body = fixture("rest_markets.json");
    static const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& one : *rows) {
        if (one.symbol == symbol) {
            return one;
        }
    }
    ADD_FAILURE() << "no market " << symbol;
    return {};
}

InstrumentBase base_for(std::string_view symbol) {
    const auto instrument = map_instrument(row(symbol), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Arcus, MarketType::Perp, SymbolId{2});
    return base;
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

TEST(ArcusCatalogue, LoadsTheOnlinePerpetualsWithTheirAssetClass) {
    const auto body = fixture("rest_markets.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 6U);
    std::size_t loaded = 0;
    for (const auto& one : *rows) {
        loaded += map_instrument(one, MarketType::Perp).has_value() ? 1U : 0U;
        EXPECT_FALSE(map_instrument(one, MarketType::Spot).has_value());
    }
    EXPECT_EQ(loaded, 5U);
    EXPECT_FALSE(map_instrument(row("F-USD"), MarketType::Perp).has_value());

    const auto btc = base_for("BTC-USD");
    EXPECT_EQ(btc.exchange_symbol.view(), "BTC-USD");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(btc.tick_size, e9("0.1"));
    EXPECT_EQ(btc.lot_size, e9("0.00000001"));
    EXPECT_EQ(btc.min_notional, e9("5"));
    EXPECT_EQ(btc.contract_value, e9("1"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(btc.asset_class, AssetClass::Crypto);

    const auto instrument = map_instrument(row("BTC-USD"), MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 3600U);
}

TEST(ArcusCatalogue, StocksAndFundsKeyOnTheirTicker) {
    for (const auto* symbol : {"NVDA-USD", "SPY-USD", "GLD-USD"}) {
        auto base = base_for(symbol);
        EXPECT_EQ(base.asset_class, AssetClass::Equity) << symbol;
        classify(VenueId::Arcus, base);
        const std::string ticker{std::string_view{symbol}.substr(0, std::string_view{symbol}.find('-'))};
        EXPECT_EQ(asset_key(base), "equity:" + ticker);
    }
    auto eth = base_for("ETH-USD");
    classify(VenueId::Arcus, eth);
    EXPECT_EQ(asset_key(eth), "crypto:ETH");
    EXPECT_TRUE(states_asset_class(VenueId::Arcus));
}

TEST(ArcusFrames, HeadsNameTheirChannelAndKind) {
    const auto book_text = fixture("ws_subscribed_book.json");
    const auto book = parse_head(book_text);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->type, FrameType::Subscribed);
    EXPECT_EQ(book->channel, Channel::Book);
    EXPECT_EQ(book->id, "ETH-USD");

    const auto degraded_text = fixture("ws_degraded.json");
    const auto degraded = parse_head(degraded_text);
    ASSERT_TRUE(degraded.has_value());
    EXPECT_EQ(degraded->type, FrameType::Degraded);
    EXPECT_EQ(degraded->channel, Channel::Book);

    const auto refused_text = fixture("ws_error_unknown_channel.json");
    const auto refused = parse_head(refused_text);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->type, FrameType::Error);
    EXPECT_TRUE(refused->message.starts_with("Unknown channel"));

    const auto lapsed_text = fixture("ws_error_not_subscribed.json");
    const auto lapsed = parse_head(lapsed_text);
    ASSERT_TRUE(lapsed.has_value());
    EXPECT_TRUE(lapsed->message.starts_with("Not subscribed"));

    const auto hello_text = fixture("ws_connected.json");
    const auto hello = parse_head(hello_text);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, FrameType::Connected);
    EXPECT_EQ(parse_head(fixture("ws_markets.json"))->channel, Channel::Markets);
}

TEST(ArcusQuotes, BboCarriesTheVenueStampInMicroseconds) {
    const auto btc = base_for("BTC-USD");
    const auto wire_text = fixture("ws_bbo.json");
    const auto wire = parse_bbo(wire_text);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, btc, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid, PriceTicks::from_raw(864631));
    EXPECT_EQ(bbo->ask, PriceTicks::from_raw(864632));
    EXPECT_EQ(bbo->bid_qty, QtyLots::from_raw(644919827));
    EXPECT_EQ(bbo->ts_event, UnixNanos{1'791'299'087'066'598'000ULL});

    const auto ack_text = fixture("ws_subscribed_bbo.json");
    const auto ack = parse_bbo(ack_text);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->seq, 329976766U);
    EXPECT_TRUE(map_bbo(*ack, btc, k_ingest).has_value());
}

TEST(ArcusTrades, SideIsTheTakersAndIdsAreNumbers) {
    const auto btc = base_for("BTC-USD");
    const auto storage = std::make_unique<PrintBuffer>();
    const auto rows_text = fixture("ws_trades.json");
    const auto rows = parse_trades(rows_text, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_GT(rows->size(), 1U);
    EXPECT_EQ(rows->front().trade_id, 12836683U);
    const auto trade = map_trade(rows->front(), btc, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(864631));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(17763));
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'299'090'219'027'000ULL});

    const auto none_text = fixture("ws_subscribed_trades.json");
    const auto none = parse_trades(none_text, *storage);
    EXPECT_FALSE(none.has_value());
}

TEST(ArcusFunding, MarketsSnapshotGivesRateEstimateMarkAndHour) {
    const auto storage = std::make_unique<StatsBuffer>();
    const auto rows_text = fixture("ws_markets.json");
    const auto rows = parse_stats(rows_text, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 6U);
    const auto* spy = &rows->front();
    for (const auto& one : *rows) {
        if (one.symbol == "SPY-USD") {
            spy = &one;
        }
    }
    ASSERT_EQ(spy->symbol, "SPY-USD");
    const auto base = base_for("SPY-USD");
    const auto funding = map_funding(*spy, base, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, FixedE9::from_raw(-20404));
    EXPECT_EQ(funding->predicted_rate, FixedE9::from_raw(-19760));
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(78061));
    EXPECT_EQ(funding->index_price, PriceTicks::from_raw(78058));
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'791'302'400'000'000'000ULL});
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Discrete);
    EXPECT_EQ(funding->ts_event, UnixNanos{});

    const auto stats = map_ticker_stats(*spy, base, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("65242677.41").value());
    EXPECT_EQ(stats->open_interest, FixedE9::from_raw(8'912'756'729'400));
    EXPECT_EQ(stats->ts_event, UnixNanos{});
}

TEST(ArcusBook, SnapshotThenDeltasWithTheBoundaryGapTolerated) {
    const auto eth = base_for("ETH-USD");
    Book book{eth.id, eth.tick_size, eth.lot_size};
    const auto storage = std::make_unique<LevelBuffer>();

    const auto snapshot_text = fixture("ws_subscribed_book.json");
    const auto snapshot = parse_book(snapshot_text, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->bids.size(), 20U);
    const auto base_seq = snapshot->seq;
    EXPECT_EQ(book.apply(*snapshot), BookResult::Ignored);
    EXPECT_EQ(book.reset(*snapshot), BookResult::Resynced);
    const auto top = book.snapshot(UnixNanos{}, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bids[0].price, PriceTicks::from_raw(271700));
    EXPECT_EQ(top->ts_event, UnixNanos{});

    const auto delta_text = fixture("ws_book_delta.json");
    const auto at = [&](std::uint64_t seq) { return replaced(delta_text, "253204666", std::to_string(seq)); };
    const auto stale = at(base_seq);
    EXPECT_EQ(book.apply(*parse_book(stale, *storage)), BookResult::Ignored);

    const auto first = at(base_seq + 3);
    EXPECT_EQ(book.apply(*parse_book(first, *storage)), BookResult::Applied);
    const auto after = book.snapshot(UnixNanos{}, k_ingest);
    ASSERT_TRUE(after.has_value());
    for (std::uint32_t i = 0; i < after->bid_count; ++i) {
        EXPECT_NE(after->bids[i].price, PriceTicks::from_raw(271693));
    }
    EXPECT_EQ(after->venue_seq, base_seq + 3);

    const auto next = at(base_seq + 4);
    EXPECT_EQ(book.apply(*parse_book(next, *storage)), BookResult::Applied);

    const auto gap = at(base_seq + 6);
    EXPECT_EQ(book.apply(*parse_book(gap, *storage)), BookResult::Lost);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(UnixNanos{}, k_ingest).has_value());
    EXPECT_TRUE(book.take_resync(k_ingest.ns));
    EXPECT_FALSE(book.take_resync(k_ingest.ns + 1));

    EXPECT_EQ(book.reset(*parse_book(snapshot_text, *storage)), BookResult::Resynced);
    EXPECT_TRUE(book.synced());
}

TEST(ArcusBook, ALevelOffTheTickDropsTheBook) {
    const auto eth = base_for("ETH-USD");
    Book book{eth.id, eth.tick_size, eth.lot_size};
    const auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_text = fixture("ws_subscribed_book.json");
    ASSERT_EQ(book.reset(*parse_book(snapshot_text, *storage)), BookResult::Resynced);
    const auto off = replaced(
        replaced(fixture("ws_book_delta.json"), "2716.93", "2716.935"), "253204666", std::to_string(book.seq() + 1));
    EXPECT_EQ(book.apply(*parse_book(off, *storage)), BookResult::Lost);
    EXPECT_FALSE(book.synced());
}

TEST(ArcusVenue, ClockReadsNanosecondsAndCapsAreMarketDataOnly) {
    EXPECT_EQ(exec::json_number(fixture("rest_time.json"), "timeNs"), 1'791'299'043'815'670'873ULL);
    const auto& caps = exec::caps_of(VenueId::Arcus);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.spot);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.position_pull.any());
    EXPECT_EQ(caps.headline_order_rate_per_s, 0U);
    EXPECT_EQ(to_string(VenueId::Arcus), "arcus");
    EXPECT_EQ(static_cast<int>(VenueId::Arcus), 35);
}

} // namespace
} // namespace raikiri::venues::arcus
