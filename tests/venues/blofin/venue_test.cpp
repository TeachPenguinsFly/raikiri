// Against bytes captured live on 2026-09-25; see fixtures/README.md.

#include <raikiri/venues/blofin/book.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/map.hpp>
#include <raikiri/venues/blofin/market_data.hpp>
#include <raikiri/venues/blofin/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace raikiri::venues::blofin {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'790'315'400'000'000'000ULL};

FixedE9 e9(std::string_view text) {
    const auto value = parse_decimal_e9(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value.value_or(FixedE9{});
}

WireInstrument instrument_row(std::string_view inst_id) {
    static const auto body = fixture("rest_instruments.json");
    static InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.inst_id == inst_id) {
            return row;
        }
    }
    ADD_FAILURE() << "no instrument " << inst_id;
    return {};
}

WireFundingRow funding_row(std::string_view inst_id) {
    static const auto body = fixture("rest_funding_rate.json");
    static FundingBuffer storage;
    const auto rows = parse_funding(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.inst_id == inst_id) {
            return row;
        }
    }
    ADD_FAILURE() << "no funding row " << inst_id;
    return {};
}

PerpInstrument perp_of(std::string_view inst_id) {
    const auto instrument = map_instrument(instrument_row(inst_id), MarketType::Perp);
    EXPECT_TRUE(instrument.has_value()) << inst_id;
    auto perp = std::get<PerpInstrument>(instrument.value_or(PerpInstrument{}));
    perp.base.id = InstrumentId::make(VenueId::Blofin, MarketType::Perp, SymbolId{7});
    return perp;
}

TEST(BlofinCatalogue, EveryRowParses) {
    InstrumentBuffer storage;
    const auto body = fixture("rest_instruments.json");
    const auto rows = parse_instruments(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 7U);
    for (const auto& row : *rows) {
        EXPECT_EQ(row.inst_type, "SWAP") << row.inst_id;
        EXPECT_EQ(row.state, "live") << row.inst_id;
    }
}

TEST(BlofinCatalogue, ALinearPerpCountsItsContractInTheBase) {
    const auto perp = perp_of("BTC-USDT");
    EXPECT_EQ(perp.base.exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USDT");
    EXPECT_EQ(perp.base.kind, ContractKind::Linear);
    EXPECT_EQ(perp.base.tick_size, e9("0.1"));
    EXPECT_EQ(perp.base.lot_size, e9("0.1"));
    EXPECT_EQ(perp.base.contract_value, e9("0.001"));
    EXPECT_EQ(perp.funding_interval_s, 0U);

    const auto usdc = perp_of("BTC-USDC");
    EXPECT_EQ(usdc.base.local_symbol.view(), "BTC/USDC");
    EXPECT_EQ(usdc.base.contract_value, e9("0.0001"));
}

TEST(BlofinCatalogue, AnInverseContractIsOneDollar) {
    const auto perp = perp_of("BTC-USD");
    EXPECT_EQ(perp.base.kind, ContractKind::Inverse);
    EXPECT_EQ(perp.base.local_symbol.view(), "BTC/USD");
    EXPECT_EQ(perp.base.contract_value, e9("1"));
}

TEST(BlofinCatalogue, AMultiplierInTheNameStaysInTheBase) {
    const auto perp = perp_of("1000BONK-USDT");
    EXPECT_EQ(perp.base.local_symbol.view(), "1000BONK/USDT");
    EXPECT_EQ(perp.base.contract_value, e9("1000"));
    EXPECT_EQ(perp.base.tick_size, e9("0.000001"));
    EXPECT_EQ(perp_of("NVDA-USDT").base.local_symbol.view(), "NVDA/USDT");
}

TEST(BlofinCatalogue, SpotAndFuturesAreNotThisAdapters) {
    EXPECT_FALSE(map_instrument(instrument_row("BTC-USDT"), MarketType::Spot).has_value());
    EXPECT_FALSE(map_instrument(instrument_row("BTC-USDT"), MarketType::Future).has_value());
}

TEST(BlofinCatalogue, AnUnknownContractTypeIsRefused) {
    auto row = instrument_row("BTC-USDT");
    row.contract_type = "quanto";
    EXPECT_FALSE(map_instrument(row, MarketType::Perp).has_value());
}

TEST(BlofinCatalogue, TheFundingTableStatesEachContractsPeriod) {
    auto btc = perp_of("BTC-USDT");
    apply_schedule(btc, funding_row("BTC-USDT"));
    EXPECT_EQ(btc.funding_interval_s, 8U * 3600);
    EXPECT_EQ(btc.funding_cap, e9("0.005"));
    EXPECT_EQ(btc.funding_floor, e9("-0.005"));

    auto bonk = perp_of("1000BONK-USDT");
    apply_schedule(bonk, funding_row("1000BONK-USDT"));
    EXPECT_EQ(bonk.funding_interval_s, 4U * 3600);

    auto oil = perp_of("WTIOIL-USDT");
    apply_schedule(oil, funding_row("WTIOIL-USDT"));
    EXPECT_EQ(oil.funding_interval_s, 3600U);
}

TEST(BlofinCatalogue, APeriodThatCannotBeReadIsUnstated) {
    EXPECT_EQ(interval_of("8", "hour"), 28'800U);
    EXPECT_EQ(interval_of("30", "minute"), 1'800U);
    EXPECT_EQ(interval_of("1", "day"), 0U);
    EXPECT_EQ(interval_of("", "hour"), 0U);
    EXPECT_EQ(interval_of("8h", "hour"), 0U);
    EXPECT_EQ(interval_of("0", "hour"), 0U);
}

TEST(BlofinFunding, ATwentyDecimalRateIsTruncatedNotRefused) {
    FundingMemo memo{};
    ASSERT_TRUE(note_funding(memo, funding_row("BTC-USD")));
    // "0.00013053258751057685"
    EXPECT_EQ(memo.rate, FixedE9::from_raw(130'532));
    EXPECT_EQ(memo.interval_s, 8U * 3600);
    EXPECT_EQ(memo.next_funding, UnixNanos{1'790'323'200'000ULL * 1'000'000ULL});
}

TEST(BlofinFunding, TheSocketsRowKeepsThePeriodTheTableGave) {
    FundingBuffer storage;
    const auto body = fixture("ws_funding_rate.json");
    const auto rows = parse_funding(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_TRUE((*rows)[0].interval.empty());

    FundingMemo memo{.interval_s = 8U * 3600};
    ASSERT_TRUE(note_funding(memo, (*rows)[0]));
    // "-0.000060750000000000006"
    EXPECT_EQ(memo.rate, FixedE9::from_raw(-60'750));
    EXPECT_EQ(memo.interval_s, 8U * 3600);
    EXPECT_EQ(memo.next_funding, UnixNanos{1'790'323'200'000ULL * 1'000'000ULL});
}

TEST(BlofinFunding, APeriodTheVenueChangedIsTaken) {
    FundingMemo memo{.interval_s = 4U * 3600};
    ASSERT_TRUE(note_funding(memo, funding_row("ONE-USDT")));
    EXPECT_EQ(memo.interval_s, 3600U);
}

TEST(BlofinFunding, AnUnreadablePeriodClearsTheOldOne) {
    FundingMemo memo{.interval_s = 8U * 3600};
    auto row = funding_row("BTC-USDT");
    row.interval_unit = "day";
    ASSERT_TRUE(note_funding(memo, row));
    EXPECT_EQ(memo.interval_s, 0U);
}

TEST(BlofinFunding, ARowWithoutARateIsNotARateOfZero) {
    FundingMemo memo{};
    auto row = funding_row("BTC-USDT");
    row.funding_rate = {};
    EXPECT_FALSE(note_funding(memo, row));
    EXPECT_TRUE(memo.rate.is_undef());
}

TEST(BlofinFunding, TheMarkJoinsTheRecord) {
    MarkBuffer storage;
    const auto body = fixture("rest_mark_price.json");
    const auto rows = parse_mark_table(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);

    const auto btc = perp_of("BTC-USDT");
    FundingMemo memo{};
    ASSERT_TRUE(note_funding(memo, funding_row("BTC-USDT")));
    ASSERT_TRUE(note_mark(memo, (*rows)[0], btc.base));
    const auto update = merged(btc.base, memo, k_ingest, k_ingest);
    EXPECT_EQ(update.instrument, btc.base.id);
    EXPECT_EQ(update.rate, FixedE9::from_raw(-61'270));
    EXPECT_EQ(update.mark_price, PriceTicks::from_raw(842'356));
    EXPECT_EQ(update.interval_s, 8U * 3600);
    EXPECT_TRUE(update.predicted_rate.is_undef());
    EXPECT_EQ(update.next_funding_time, UnixNanos{1'790'323'200'000ULL * 1'000'000ULL});
}

struct Chain {
    LevelBuffer levels;
    std::string text;
    WireBooks wire;
};

std::unique_ptr<Chain> books(std::string_view name) {
    auto out = std::make_unique<Chain>();
    out->text = fixture(name);
    const auto wire = parse_books(out->text, out->levels);
    EXPECT_TRUE(wire.has_value()) << name;
    out->wire = wire.value_or(WireBooks{});
    return out;
}

TEST(BlofinBook, TheSnapshotChainsOnStringSequences) {
    const auto snap = books("ws_books_snapshot.json");
    EXPECT_TRUE(snap->wire.snapshot);
    EXPECT_EQ(snap->wire.inst_id, "WTIOIL-USDT");
    EXPECT_EQ(snap->wire.prev_seq_id, 0U);
    EXPECT_EQ(snap->wire.seq_id, 761'917'822U);
    EXPECT_EQ(snap->wire.bids.size(), 400U);
    EXPECT_EQ(snap->wire.asks.size(), 400U);
    const auto next = books("ws_books_update.json");
    EXPECT_FALSE(next->wire.snapshot);
    EXPECT_EQ(next->wire.prev_seq_id, 761'917'822U);
    EXPECT_EQ(next->wire.seq_id, 761'917'824U);
}

TEST(BlofinBook, DeltasApplyAndMoveTheTouch) {
    const auto oil = perp_of("WTIOIL-USDT");
    Book book{oil.base.id, oil.base.tick_size, oil.base.lot_size};
    ASSERT_EQ(book.apply(books("ws_books_snapshot.json")->wire), BookResult::Resynced);
    auto top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(9'248));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(9'251));

    // The first delta deletes the best bid with a size of "0".
    ASSERT_EQ(book.apply(books("ws_books_update.json")->wire), BookResult::Applied);
    ASSERT_EQ(book.apply(books("ws_books_update_next.json")->wire), BookResult::Applied);
    top = book.top(k_ingest, k_ingest);
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->bid, PriceTicks::from_raw(9'246));
    EXPECT_EQ(top->bid_qty, QtyLots::from_raw(200'000));
    EXPECT_EQ(top->ask, PriceTicks::from_raw(9'249));
    EXPECT_EQ(top->ask_qty, QtyLots::from_raw(899));
    const auto depth = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->venue_seq, 761'917'826U);
}

TEST(BlofinBook, ARedeliveredDeltaIsIgnored) {
    const auto oil = perp_of("WTIOIL-USDT");
    Book book{oil.base.id, oil.base.tick_size, oil.base.lot_size};
    ASSERT_EQ(book.apply(books("ws_books_snapshot.json")->wire), BookResult::Resynced);
    const auto update = books("ws_books_update.json");
    ASSERT_EQ(book.apply(update->wire), BookResult::Applied);
    EXPECT_EQ(book.apply(update->wire), BookResult::Ignored);
    EXPECT_TRUE(book.synced());
    EXPECT_EQ(book.gaps(), 0U);
}

TEST(BlofinBook, AMissingDeltaDarkensTheBook) {
    const auto oil = perp_of("WTIOIL-USDT");
    Book book{oil.base.id, oil.base.tick_size, oil.base.lot_size};
    const auto skipped = books("ws_books_update_next.json");
    EXPECT_EQ(book.apply(skipped->wire), BookResult::Ignored);
    ASSERT_EQ(book.apply(books("ws_books_snapshot.json")->wire), BookResult::Resynced);
    EXPECT_EQ(book.apply(skipped->wire), BookResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.top(k_ingest, k_ingest).has_value());
    EXPECT_EQ(book.apply(skipped->wire), BookResult::Ignored);
}

TEST(BlofinBook, ALevelOffTheGridDropsTheBook) {
    const auto oil = perp_of("WTIOIL-USDT");
    Book book{oil.base.id, e9("0.1"), oil.base.lot_size};
    EXPECT_EQ(book.apply(books("ws_books_snapshot.json")->wire), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
}

TEST(BlofinTrades, SideIsTheTakers) {
    PrintBuffer storage;
    const auto body = fixture("ws_trades.json");
    const auto prints = parse_trades(body, storage);
    ASSERT_TRUE(prints.has_value());
    ASSERT_EQ(prints->size(), 1U);
    const auto btc = perp_of("BTC-USDT");
    const auto trade = map_trade((*prints)[0], btc.base, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
    EXPECT_EQ(trade->price, PriceTicks::from_raw(842'327));
    EXPECT_EQ(trade->qty, QtyLots::from_raw(110)); // 11 contracts on a 0.1 lot
    EXPECT_EQ(trade->ts_event, UnixNanos{1'790'315'330'244ULL * 1'000'000ULL});
}

TEST(BlofinTickers, ALinearContractStatesItsBaseVolume) {
    TickerBuffer storage;
    const auto body = fixture("ws_tickers.json");
    const auto rows = parse_tickers(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto stats = map_ticker_stats((*rows)[0], perp_of("BTC-USDT").base, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base, e9("4072.6116"));
    EXPECT_TRUE(stats->volume_24h_quote.is_undef());
}

TEST(BlofinTickers, AnInverseContractStatesDollarsInBothColumns) {
    TickerBuffer storage;
    const auto body = fixture("ws_tickers_inverse.json");
    const auto rows = parse_tickers(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ((*rows)[0].vol_contracts, (*rows)[0].vol_currency);
    const auto stats = map_ticker_stats((*rows)[0], perp_of("BTC-USD").base, k_ingest);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_quote, e9("22426621"));
    EXPECT_TRUE(stats->volume_24h_base.is_undef());
}

TEST(BlofinEnvelope, FramesAreSortedByChannel) {
    const auto kind = [](std::string_view name) {
        const auto body = fixture(name);
        const auto event = parse_event(body);
        EXPECT_TRUE(event.has_value()) << name;
        return event.value_or(EventInfo{}).kind;
    };
    EXPECT_EQ(kind("ws_books_snapshot.json"), EventKind::Books);
    EXPECT_EQ(kind("ws_trades.json"), EventKind::Trades);
    EXPECT_EQ(kind("ws_tickers.json"), EventKind::Tickers);
    EXPECT_EQ(kind("ws_funding_rate.json"), EventKind::FundingRate);
    EXPECT_EQ(kind("ws_subscribe_ack.json"), EventKind::SubscribeAck);
    EXPECT_EQ(kind("ws_error_invalid_request.json"), EventKind::Error);
    EXPECT_FALSE(parse_event("pong").has_value());
}

TEST(BlofinEnvelope, ARefusalNamesTheInstrumentItsFrameCarried) {
    const auto body = fixture("ws_error_invalid_request.json");
    const auto err = parse_stream_error(body);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_invalid_request);
    EXPECT_EQ(err->inst_id, "NOPE-USDT");
    EXPECT_EQ(err->args, 4U);
}

TEST(BlofinEnvelope, TheRestCodeIsAString) {
    const auto ok = parse_rest_envelope(fixture("rest_mark_price.json"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);
    const auto refused =
        parse_rest_envelope(R"({"code":"152002","msg":"Parameter instId error."})");
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->code, 152'002);
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"","msg":""})").has_value());
}

TEST(BlofinProtocol, OneFrameCarriesOneInstrument) {
    Protocol protocol;
    const std::array<std::string_view, 2> channels{k_books_channel, k_trades_channel};
    const auto on = protocol.subscribe_frame("BTC-USDT", channels);
    ASSERT_TRUE(on.has_value());
    EXPECT_EQ(*on,
              R"({"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"},)"
              R"({"channel":"trades","instId":"BTC-USDT"}]})");
    const std::array<std::string_view, 1> book{k_books_channel};
    const auto off = protocol.unsubscribe_frame("BTC-USDT", book);
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off, R"({"op":"unsubscribe","args":[{"channel":"books","instId":"BTC-USDT"}]})");
    EXPECT_FALSE(protocol.subscribe_frame("", channels).has_value());
}

} // namespace
} // namespace raikiri::venues::blofin
