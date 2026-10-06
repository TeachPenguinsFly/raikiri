#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/venue_clock.hpp>
#include <raikiri/venues/paradex/book.hpp>
#include <raikiri/venues/paradex/map.hpp>
#include <raikiri/venues/paradex/wire.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::paradex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'791'300'200'000'000'000ULL};

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
    base.id = InstrumentId::make(VenueId::Paradex, MarketType::Perp, SymbolId{4});
    return base;
}

// the book frame's seq sits right after its timestamp
std::string with_seq(std::string frame, std::int64_t seq) {
    std::memcpy(frame.data() + 16, &seq, sizeof(seq));
    return frame;
}

TEST(ParadexCatalogue, KeepsPerpetualsAndLeavesSpotAndOptionsOut) {
    const auto body = fixture("rest_markets.json");
    const auto storage = std::make_unique<MarketBuffer>();
    const auto rows = parse_markets(body, *storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(storage->listed, 8U);
    ASSERT_EQ(rows->size(), 7U);
    std::size_t perps = 0;
    for (const auto& one : *rows) {
        perps += map_instrument(one, MarketType::Perp).has_value() ? 1U : 0U;
    }
    EXPECT_EQ(perps, 6U);
    EXPECT_FALSE(map_instrument(row("ETH-USD"), MarketType::Perp).has_value());

    const auto btc = base_for("BTC-USD-PERP");
    EXPECT_EQ(btc.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(btc.tick_size, e9("0.1"));
    EXPECT_EQ(btc.lot_size, e9("0.00001"));
    EXPECT_EQ(btc.min_notional, e9("10"));
    EXPECT_EQ(btc.kind, ContractKind::Linear);
    EXPECT_EQ(btc.asset_class, AssetClass::Crypto);

    const auto perp = std::get<PerpInstrument>(*map_instrument(row("BTC-USD-PERP"), MarketType::Perp));
    EXPECT_EQ(perp.funding_interval_s, 0U);
    EXPECT_EQ(perp.funding_cap, e9("0.02"));
    EXPECT_EQ(perp.funding_floor, e9("-0.02"));
}

TEST(ParadexCatalogue, RwaRowsSplitIntoStocksIndicesAndCommodities) {
    auto nvda = base_for("NVDA-USD-PERP");
    auto xau = base_for("XAU-USD-PERP");
    auto spx = base_for("US500-USD-PERP");
    auto pepe = base_for("kPEPE-USD-PERP");
    for (auto* base : {&nvda, &xau, &spx, &pepe}) {
        classify(VenueId::Paradex, *base);
    }
    EXPECT_EQ(asset_key(nvda), "equity:NVDA");
    EXPECT_EQ(asset_key(xau), "commodity:XAU");
    EXPECT_EQ(asset_key(spx), "index:SPX");
    EXPECT_EQ(pepe.asset_class, AssetClass::Crypto);
    EXPECT_TRUE(states_asset_class(VenueId::Paradex));
}

TEST(ParadexFrames, JsonRepliesAreNotSbeAndSbeFramesNameTheirTemplate) {
    const auto ok_text = fixture("ws_reply_subscribed.json");
    const auto refused_text = fixture("ws_reply_error.json");
    EXPECT_FALSE(is_sbe(ok_text));
    EXPECT_FALSE(is_sbe(refused_text));
    const auto ok = parse_reply(ok_text);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, 0);
    EXPECT_EQ(ok->channel, "order_book.BTC-USD-PERP.deltas");
    const auto refused = parse_reply(refused_text);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->code, -32600);

    EXPECT_EQ(template_of(fixture("sbe_book_snapshot.bin")), Template::Book);
    EXPECT_EQ(template_of(fixture("sbe_bbo.bin")), Template::Bbo);
    EXPECT_EQ(template_of(fixture("sbe_trade.bin")), Template::Trade);
    EXPECT_EQ(template_of(fixture("sbe_summary.bin")), Template::Summary);
    EXPECT_EQ(template_of(fixture("sbe_funding.bin")), Template::Funding);
    EXPECT_FALSE(parse_bbo(fixture("sbe_trade.bin")).has_value());
    const auto cut = fixture("sbe_bbo.bin").substr(0, 40);
    EXPECT_FALSE(parse_bbo(cut).has_value());
}

TEST(ParadexBook, SnapshotThenContiguousDeltasEmptyOnesIncluded) {
    const auto eth = base_for("ETH-USD-PERP");
    Book book{eth};
    const auto storage = std::make_unique<LevelBuffer>();

    const auto snapshot_text = fixture("sbe_book_snapshot.bin");
    const auto snapshot = parse_book(snapshot_text, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->market, "ETH-USD-PERP");
    EXPECT_EQ(snapshot->bids.size(), 125U);
    EXPECT_EQ(snapshot->asks.size(), 29U);
    const auto base_seq = snapshot->seq;
    EXPECT_EQ(book.on_frame(*snapshot), BookResult::Resynced);
    const auto top = book.top(us_to_unix(snapshot->ts_us), k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(271980));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(272284));
    EXPECT_EQ(top->ts_event, UnixNanos{1'791'300'108'263'000'000ULL});

    const auto empty = with_seq(fixture("sbe_book_delta_empty.bin"), base_seq + 1);
    const auto quiet = parse_book(empty, *storage);
    ASSERT_TRUE(quiet.has_value());
    EXPECT_TRUE(quiet->bids.empty() && quiet->asks.empty());
    EXPECT_EQ(book.on_frame(*quiet), BookResult::Applied);

    const auto stale = with_seq(fixture("sbe_book_delta.bin"), base_seq);
    EXPECT_EQ(book.on_frame(*parse_book(stale, *storage)), BookResult::Ignored);

    const auto removal = with_seq(fixture("sbe_book_delta.bin"), base_seq + 2);
    EXPECT_EQ(book.on_frame(*parse_book(removal, *storage)), BookResult::Applied);
    const auto after = book.top(UnixNanos{}, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bid, PriceTicks::from_raw(271931));

    const auto gap = with_seq(fixture("sbe_book_delta_empty.bin"), base_seq + 4);
    EXPECT_EQ(book.on_frame(*parse_book(gap, *storage)), BookResult::Lost);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.top(UnixNanos{}, k_ingest).has_value());
    EXPECT_TRUE(book.take_resync(k_ingest.ns));
    EXPECT_FALSE(book.take_resync(k_ingest.ns + 1));
    EXPECT_EQ(book.on_frame(*parse_book(snapshot_text, *storage)), BookResult::Resynced);
}

TEST(ParadexBook, BboFrameMatchesTheBookItRides) {
    const auto wire_text = fixture("sbe_bbo.bin");
    const auto wire = parse_bbo(wire_text);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->market, "ETH-USD-PERP");
    EXPECT_EQ(wire->seq, 8105814772);
    EXPECT_EQ(e8_to_e9(wire->bid), e9("2719.31"));
    EXPECT_EQ(e8_to_e9(wire->ask_qty), e9("0.0148"));
    EXPECT_FALSE(e8_to_e9(k_null).has_value());
}

TEST(ParadexTrades, TakerSideAndTheFillTimestamp) {
    const auto btc = base_for("BTC-USD-PERP");
    const auto wire_text = fixture("sbe_trade.bin");
    const auto wire = parse_trade(wire_text);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->market, "BTC-USD-PERP");
    EXPECT_EQ(wire->type, 1U);
    const auto trade = map_trade(*wire, btc, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(865208));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(15));
    EXPECT_EQ(trade->ts_event, UnixNanos{1'791'300'131'096'000'000ULL});

    const auto rpi_text = fixture("sbe_trade_rpi.bin");
    const auto rpi = parse_trade(rpi_text);
    ASSERT_TRUE(rpi.has_value());
    EXPECT_EQ(rpi->type, 5U);
    EXPECT_EQ(rpi->market, "US500-USD-PERP");
    const auto option_text = fixture("sbe_trade_option.bin");
    const auto option = parse_trade(option_text);
    ASSERT_TRUE(option.has_value());
    EXPECT_EQ(option->type, 6U);
}

TEST(ParadexFunding, ContinuousRateOverEightHoursWithTheSummaryMark) {
    const auto btc = base_for("BTC-USD-PERP");
    const auto summary_text = fixture("sbe_summary.bin");
    const auto summary = parse_summary(summary_text);
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->market, "BTC-USD-PERP");
    EXPECT_EQ(e8_to_e9(summary->index), e9("86583.00648617"));
    EXPECT_EQ(e8_to_e9(summary->open_interest), e9("84.41064"));

    const auto wire_text = fixture("sbe_funding.bin");
    const auto wire = parse_funding(wire_text);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->period_hours, 8);
    const auto funding = map_funding(*wire, btc, summary->mark, summary->index, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate, e9("0.00008867"));
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->mark_price, PriceTicks::from_raw(866127));
    EXPECT_EQ(funding->index_price, PriceTicks::from_raw(865830));
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->accrual, FundingAccrual::Continuous);
    EXPECT_EQ(funding->next_funding_time, UnixNanos{});
    EXPECT_EQ(funding->ts_event, UnixNanos{1'791'299'676'281'000'000ULL});
    EXPECT_EQ(map_funding(*wire, btc, k_null, k_null, k_ingest)->mark_price, FundingUpdate{}.mark_price);

    const auto stats = map_ticker_stats(*summary, btc, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, parse_turnover_e6("1510190.431717").value());
    EXPECT_EQ(stats->open_interest, e9("84.41064"));
}

TEST(ParadexVenue, ClockReadsMillisecondsAndCapsAreMarketDataOnly) {
    EXPECT_EQ(exec::json_number(fixture("rest_time.json"), "server_time", 1'000'000), 1'791'299'596'588'000'000ULL);
    const auto& caps = exec::caps_of(VenueId::Paradex);
    EXPECT_TRUE(caps.markets.perp);
    EXPECT_FALSE(caps.markets.option);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_FALSE(caps.order_pull.any());
    EXPECT_EQ(to_string(VenueId::Paradex), "paradex");
    EXPECT_EQ(static_cast<int>(VenueId::Paradex), 37);
}

} // namespace
} // namespace raikiri::venues::paradex
