#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/bullish/map.hpp>
#include <raikiri/venues/bullish/wire.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::bullish {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'791'300'400'000'000'000ULL};

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

InstrumentBase base_for(std::string_view symbol, MarketType market) {
    const auto instrument = map_instrument(row(symbol), market);
    EXPECT_TRUE(instrument.has_value()) << symbol;
    auto base = base_of(*instrument);
    base.id = InstrumentId::make(VenueId::Bullish, market, SymbolId{5});
    return base;
}

TEST(BullishCatalogue, SpotAndEnabledPerpetualsOnTheirPrintGrid) {
    const auto body = fixture("rest_markets.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 9U);
    std::size_t spot = 0;
    std::size_t perp = 0;
    for (const auto& one : *rows) {
        spot += map_instrument(one, MarketType::Spot).has_value() ? 1U : 0U;
        perp += map_instrument(one, MarketType::Perp).has_value() ? 1U : 0U;
    }
    EXPECT_EQ(spot, 3U);
    EXPECT_EQ(perp, 3U);
    EXPECT_FALSE(map_instrument(row("AEVO-USDC-PERP"), MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument(row("BTC-USDC-20261007"), MarketType::Perp).has_value());

    const auto btc = base_for("BTCUSDC", MarketType::Spot);
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(btc.tick_size, e9("0.0001"));
    EXPECT_EQ(btc.lot_size, e9("0.00000001"));

    const auto perp_btc = base_for("BTC-USDC-PERP", MarketType::Perp);
    EXPECT_EQ(perp_btc.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(perp_btc.contract_value, e9("1"));
    EXPECT_EQ(perp_btc.kind, ContractKind::Linear);
    const auto instrument = map_instrument(row("BTC-USDC-PERP"), MarketType::Perp);
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 3600U);
}

TEST(BullishCatalogue, IndexAndOilPerpetualsAreCurated) {
    auto wti = base_for("CMWTI-USDC-PERP", MarketType::Perp);
    auto cd20 = base_for("CD20-USDC-PERP", MarketType::Perp);
    auto btc = base_for("BTC-USDC-PERP", MarketType::Perp);
    for (auto* base : {&wti, &cd20, &btc}) {
        classify(VenueId::Bullish, *base);
    }
    EXPECT_EQ(asset_key(wti), "commodity:CL");
    EXPECT_EQ(cd20.asset_class, AssetClass::Index);
    EXPECT_EQ(asset_key(btc), "crypto:BTC");
}

TEST(BullishFrames, HeadsNameTheFamily) {
    EXPECT_EQ(parse_head(fixture("ws_l2_snapshot_perp.json"))->data, DataType::Book);
    EXPECT_EQ(parse_head(fixture("ws_tick_perp.json"))->data, DataType::Tick);
    EXPECT_EQ(parse_head(fixture("ws_trades_snapshot.json"))->kind, Kind::Snapshot);
    EXPECT_EQ(parse_head(fixture("ws_trades_update.json"))->kind, Kind::Update);
    EXPECT_EQ(parse_head(fixture("ws_refusal.json"))->kind, Kind::Refusal);
    EXPECT_EQ(parse_head(fixture("ws_reply_subscribed.json"))->kind, Kind::Reply);
    EXPECT_EQ(parse_head(fixture("ws_keepalive_pong.json"))->kind, Kind::Reply);
}

TEST(BullishBook, EverySnapshotIsWholeAndTheRangesMeet) {
    const auto perp = base_for("BTC-USDC-PERP", MarketType::Perp);
    const auto storage = std::make_unique<LevelBuffer>();
    const auto first_text = fixture("ws_l2_snapshot_perp.json");
    const auto first = parse_book(first_text, *storage);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->bids.size(), 200U);
    EXPECT_EQ(first->seq_first, 1672258380U);

    SeqRange range{};
    EXPECT_TRUE(range.admit(*first));
    EXPECT_FALSE(range.admit(*first));
    const auto book = map_book(*first, perp, k_ingest);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->ts_event, UnixNanos{1'791'300'393'639'000'000ULL});
    EXPECT_EQ(book->venue_seq, 1672258380U);
    const auto top = top_of(*book);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(862212000));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(862213000));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(140669892));

    const auto next_text = fixture("ws_l2_next_perp.json");
    const auto next = parse_book(next_text, *storage);
    ASSERT_TRUE(next.has_value());
    EXPECT_TRUE(range.admit(*next));
    EXPECT_EQ(range.gaps, 0U);
    WireBook jump = *next;
    jump.seq_first += 5;
    jump.seq_last += 5;
    EXPECT_TRUE(range.admit(jump));
    EXPECT_EQ(range.gaps, 1U);

    const auto spot_text = fixture("ws_l2_snapshot_spot.json");
    const auto spot = parse_book(spot_text, *storage);
    ASSERT_TRUE(spot.has_value());
    EXPECT_TRUE(map_book(*spot, base_for("BTCUSDC", MarketType::Spot), k_ingest).has_value());
}

TEST(BullishTrades, AmmPricesSitOnThePrintGrid) {
    const auto btc = base_for("BTCUSDC", MarketType::Spot);
    const auto storage = std::make_unique<PrintBuffer>();
    const auto text = fixture("ws_trades_update.json");
    const auto rows = parse_trades(text, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_FALSE(rows->empty());
    EXPECT_EQ(rows->front().trade_id, 100132000028932561U);
    const auto trade = map_trade(rows->front(), btc, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price, PriceTicks::from_raw(862617639));
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'300'397'387'000'000ULL});
}

TEST(BullishFunding, TickRateIsInPercentAndSettlesHourly) {
    const auto perp = base_for("BTC-USDC-PERP", MarketType::Perp);
    const auto text = fixture("ws_tick_perp.json");
    const auto wire = parse_tick(text);
    ASSERT_TRUE(wire.has_value());
    IndexBuffer storage;
    const auto index_text = fixture("rest_index_prices.json");
    const auto rows = parse_index_prices(index_text, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    EXPECT_EQ(rows->front().asset, "BTC");
    const auto index = index_ticks(rows->front(), perp);
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, PriceTicks::from_raw(855920000));
    const auto funding = map_funding(*wire, perp, *index, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->index_price, PriceTicks::from_raw(855920000));
    EXPECT_EQ(funding->rate, e9("-0.00002802"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(862162143));
    EXPECT_EQ(funding->next_funding_time, UnixNanos{1'791'302'400'000'000'000ULL});
    EXPECT_EQ(funding->interval_s, 3600U);
    EXPECT_EQ(funding->ts_event, UnixNanos{1'791'300'392'420'000'000ULL});
    const auto stats = map_ticker_stats(*wire, perp, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("50346263.0723").value());
    EXPECT_EQ(stats->open_interest, e9("357.64387029"));

    const auto spot_text = fixture("ws_tick_spot.json");
    const auto spot = parse_tick(spot_text);
    ASSERT_TRUE(spot.has_value());
    EXPECT_TRUE(spot->funding_rate.empty());
    const auto spot_stats = map_ticker_stats(*spot, base_for("BTCUSDC", MarketType::Spot), k_ingest);
    ASSERT_TRUE(spot_stats.has_value());
    EXPECT_TRUE(spot_stats->open_interest.is_undef());
}

TEST(BullishVenue, ClockReadsMillisecondsAndCapsAreMarketDataOnly) {
    EXPECT_EQ(exec::json_number(fixture("rest_time.json"), "timestamp", 1'000'000), 1'791'300'318'059'000'000ULL);
    const auto& caps = exec::caps_of(VenueId::Bullish);
    EXPECT_TRUE(caps.markets.spot);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.fill_pull.any());
    EXPECT_EQ(to_string(VenueId::Bullish), "bullish");
    EXPECT_EQ(static_cast<int>(VenueId::Bullish), 39);
}

} // namespace
} // namespace raikiri::venues::bullish
