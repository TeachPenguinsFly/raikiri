// against captured bytes; see fixtures/README.md

#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/blofin/book.hpp>
#include <raikiri/venues/blofin/catalogue.hpp>
#include <raikiri/venues/blofin/envelope.hpp>
#include <raikiri/venues/blofin/map.hpp>
#include <raikiri/venues/blofin/market_data.hpp>
#include <raikiri/venues/blofin/trade.hpp>
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

TEST(BlofinCatalogue, TheAssetClassFieldIsRead) {
    const auto classified = [](const WireInstrument& row) {
        const auto instrument = map_instrument(row, MarketType::Perp);
        EXPECT_TRUE(instrument.has_value()) << row.inst_id;
        auto base = base_of(instrument.value_or(PerpInstrument{}));
        const auto stated = base.asset_class;
        classify(VenueId::Blofin, base);
        return std::pair{stated, base};
    };
    const auto [nvda_stated, nvda] = classified(instrument_row("NVDA-USDT"));
    EXPECT_EQ(nvda_stated, AssetClass::Equity);
    EXPECT_EQ(nvda.underlying.view(), "NVDA");

    const auto [oil_stated, oil] = classified(instrument_row("WTIOIL-USDT"));
    EXPECT_EQ(oil_stated, AssetClass::Commodity);
    EXPECT_EQ(oil.asset_class, AssetClass::Commodity);

    auto spx_row = instrument_row("BTC-USDT");
    spx_row.inst_id = "SPX-USDT";
    spx_row.base_ccy = "SPX";
    const auto [spx_stated, spx] = classified(spx_row);
    EXPECT_EQ(spx_stated, AssetClass::Crypto);
    EXPECT_EQ(spx.underlying.view(), "SPX");

    spx_row.asset_class = "Bonds";
    const auto [odd_stated, odd] = classified(spx_row);
    EXPECT_EQ(odd_stated, AssetClass::Unknown);
    EXPECT_EQ(odd.asset_class, AssetClass::Unknown);
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

TEST(BlofinOpenInterest, TheTableStatesItInTheBase) {
    OpenInterestBuffer storage;
    const auto body = fixture("rest_open_interest.json");
    const auto rows = parse_open_interest(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);
    EXPECT_EQ((*rows)[0].inst_id, "BTC-USDT");
    // 4007717 contracts of 0.001 BTC
    EXPECT_EQ(parse_amount_e9((*rows)[0].base), FixedE9::from_raw(4'007'717'000'000));
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
    EXPECT_EQ(update.index_price, PriceTicks::from_raw(842'807)); // 84280.7
    EXPECT_TRUE(update.predicted_rate.is_undef());
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

    // the first delta deletes the best bid with size "0"
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

TEST(BlofinBook, TheWidestLiveDiffParsesWithRoomForAWholeSideTurningOver) {
    const auto wide = books("ws_books_update_wide.json");
    EXPECT_FALSE(wide->wire.snapshot);
    EXPECT_EQ(wide->wire.inst_id, "CT-USDT");
    EXPECT_EQ(wide->wire.bids.size(), 482U);
    EXPECT_EQ(wide->wire.asks.size(), 332U);
    EXPECT_EQ(wide->wire.prev_seq_id, 217'103'695U);
    EXPECT_EQ(wide->wire.seq_id, 217'104'482U);
    EXPECT_GE(k_max_levels, 2U * books("ws_books_snapshot.json")->wire.bids.size());
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
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("343084539.14604"));
}

TEST(BlofinTickers, ALinearContractWithoutALastPrintHasNoTurnover) {
    WireTicker wire{};
    wire.inst_id = "BTC-USDT";
    wire.vol_contracts = "4072611.6";
    wire.vol_currency = "4072.6116";
    const auto stats = map_ticker_stats(wire, perp_of("BTC-USDT").base, k_ingest);
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
    EXPECT_EQ(stats->volume_24h_quote, *parse_turnover_e6("22426621"));
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
    const auto refused = parse_rest_envelope(R"({"code":"152002","msg":"Parameter instId error."})");
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

exec::InstrumentTable btc_perp_table() {
    exec::InstrumentTable table;
    EXPECT_TRUE(table.add(*map_instrument(instrument_row("BTC-USDT"), MarketType::Perp)).has_value());
    return table;
}

const InstrumentBase& btc_perp() {
    static const auto table = btc_perp_table();
    return base_of(*table.find("BTC-USDT"));
}

ExchangeClientOrderId coid() {
    return *ExchangeClientOrderId::from("01HZY3M7K8Q9R0S1T2V3W4X5Y6");
}

// the docs' examples trade ETH-USDT, which this catalogue fixture does not hold
std::string on_btc(std::string body) {
    for (auto at = body.find("ETH-USDT"); at != std::string::npos; at = body.find("ETH-USDT")) {
        body.replace(at, 8, "BTC-USDT");
    }
    return body;
}

TEST(BlofinSigner, MatchesTheVectorInBlofinsOwnTests) {
    net::Signer signer;
    signer.set_api_key("key");
    ASSERT_TRUE(signer.secret().assign("secret").has_value());
    std::array<char, 96> out{};
    // blofin-mcp tests/auth.test.ts
    EXPECT_EQ(sign_prehash(signer, "/api/v1/testGET1700000000000123e4567-e89b-12d3-a456-426614174000", out),
              "ZTQ2N2RhODY4OTExZjc1NjQyMDVhODVhZmIxOTFhYTk1OTkxODE1ZWZjNGJhNGZmOWQ1YTA2NGQ4YTE3NGUwNw==");
    // the body is signed as sent
    EXPECT_EQ(sign_prehash(signer, R"(/api/v1/trade/orderPOST1700000000000nonce-test-value{"instId":"BTC-USDT"})", out),
              "ZmRjMTZlOWY0NzBlMzU5NTUxMmYzZWJjNThmM2MzMTgzZjJkNTNmNjYwZGUzYTBhOGJjNmI4OGRjNWJhOTkxZg==");

    EXPECT_FALSE(login_frame(signer, "", UnixNanos{1'700'000'000'000'000'000ULL}).has_value());
    const auto login = login_frame(signer, "pass", UnixNanos{1'700'000'000'000'000'000ULL});
    ASSERT_TRUE(login.has_value());
    const auto sign = std::string{sign_prehash(signer, "/users/self/verifyGET17000000000001700000000000", out)};
    EXPECT_EQ(*login,
              R"({"op":"login","args":[{"apiKey":"key","passphrase":"pass","timestamp":"1700000000000","sign":")" +
                  sign + R"(","nonce":"1700000000000"}]})");
}

TEST(BlofinRequests, SignsTheBodyItSendsAndRefusesWithoutAPassphrase) {
    net::Signer signer;
    std::string passphrase;
    signer.set_api_key("key");
    ASSERT_TRUE(signer.secret().assign("secret").has_value());
    RequestBuilder builder{signer, passphrase};
    OrderCmd cmd{};
    cmd.instrument = btc_perp().id;
    cmd.side = Side::Buy;
    cmd.flags = OrderFlags::PostOnly;
    cmd.qty = QtyLots::from_raw(3);
    cmd.price = PriceTicks::from_raw(848001);
    cmd.exchange_client_order_id = coid();
    const UnixNanos now{1'700'000'000'000'000'000ULL};
    EXPECT_EQ(builder.create_order(cmd, btc_perp(), now).error(), BuildError::NoCredentials);

    passphrase = "pass";
    const auto request = builder.create_order(cmd, btc_perp(), now);
    ASSERT_TRUE(request.has_value());
    const std::string body{R"({"instId":"BTC-USDT","marginMode":"cross","positionSide":"net","side":"buy",)"
                           R"("orderType":"post_only","price":"84800.1","size":"0.3","reduceOnly":"false",)"
                           R"("clientOrderId":"01HZY3M7K8Q9R0S1T2V3W4X5Y6"})"};
    EXPECT_EQ(request->body, body);
    EXPECT_EQ(builder.last_prehash(),
              "/api/v1/trade/orderPOST1700000000000" + std::string{request->headers[3].value} + body);
    EXPECT_EQ(request->headers[4].value, "pass");
    std::array<char, 96> out{};
    EXPECT_EQ(request->headers[1].value, sign_prehash(signer, builder.last_prehash(), out));

    // a second request in the same nanosecond still gets its own nonce
    const auto again = builder.create_order(cmd, btc_perp(), now);
    ASSERT_TRUE(again.has_value());
    EXPECT_NE(again->headers[3].value, std::string_view{"1700000000000000000-1"});

    cmd.flags = OrderFlags::ReduceOnly;
    cmd.tif = TimeInForce::Ioc;
    ASSERT_TRUE(builder.create_order(cmd, btc_perp(), now).has_value());
    EXPECT_NE(builder.last_prehash().find(R"("orderType":"ioc")"), std::string_view::npos);
    EXPECT_NE(builder.last_prehash().find(R"("reduceOnly":"true")"), std::string_view::npos);

    const auto cancel = builder.cancel_order(cmd, "BTC-USDT", now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->body, R"({"instId":"BTC-USDT","clientOrderId":"01HZY3M7K8Q9R0S1T2V3W4X5Y6"})");

    const auto pending = builder.orders_pending(now);
    ASSERT_TRUE(pending.has_value());
    EXPECT_TRUE(builder.last_prehash().starts_with("/api/v1/trade/orders-pendingGET1700000000000"));
    ASSERT_TRUE(builder.order_detail("BTC-USDT", coid(), now).has_value());
    EXPECT_TRUE(builder.last_prehash().starts_with(
        "/api/v1/trade/order-detail?instId=BTC-USDT&clientOrderId=01HZY3M7K8Q9R0S1T2V3W4X5Y6GET"));
}

TEST(BlofinErrors, ReadsStringCodesAndTheOrdersOwnVerdict) {
    const auto missing_key = parse_reply(fixture("rest_error_no_such_key.json"));
    ASSERT_TRUE(missing_key.has_value());
    EXPECT_EQ(missing_key->code, k_no_such_key);
    EXPECT_EQ(classify(missing_key->code, 200), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(parse_reply(fixture("rest_place_order.json"))->code, k_ok);
    const auto refused = parse_reply(R"({"code":"0","msg":"","data":[{"orderId":"","code":"103003","msg":"x"}]})");
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->code, 103003);
    EXPECT_EQ(classify(103003, 200), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(0, 429), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_repeated_nonce, 200), exec::ErrorClass::Retryable);
    EXPECT_TRUE(is_benign_cancel_failure(k_cancel_failed));
}

TEST(BlofinUserStream, CumulativeFillsBecomeOneFillEachAndTheSnapshotOnlySeeds) {
    const auto table = btc_perp_table();
    const auto storage = std::make_unique<OrderBuffer>();
    EXPECT_EQ(parse_private(fixture("ws_login.json"), *storage)->kind, PrivateKind::Login);
    const auto failed = parse_private(fixture("ws_login_failed.json"), *storage);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->kind, PrivateKind::LoginFailed);
    EXPECT_EQ(classify(failed->code, 200), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(parse_private("pong", *storage)->kind, PrivateKind::Other);

    ProgressTable progress;
    std::array<OrderEvent, 2> events{};
    std::size_t count = 0;
    const auto snapshot_body = fixture("ws_orders_snapshot.json");
    const auto snapshot = parse_private(snapshot_body, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->kind, PrivateKind::Snapshot);
    ASSERT_EQ(snapshot->orders.size(), 1U);
    map_order_push(snapshot->orders[0], table, progress, k_ingest, events, count, true);
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(progress.count("28334314"), 1U);

    const auto push = [&](std::string_view state, std::string_view filled, std::string_view avg, std::string_view fee) {
        return std::string{R"({"action":"update","arg":{"channel":"orders"},"data":[{"instType":"SWAP",)"} +
               R"("instId":"BTC-USDT","orderId":"28334315","clientOrderId":"01HZY3M7K8Q9R0S1T2V3W4X5Y6",)" +
               R"("price":"28000.000000000000000000","size":"10","orderType":"limit","side":"buy",)" +
               R"("filledSize":")" + std::string{filled} + R"(","averagePrice":")" + std::string{avg} +
               R"(","state":")" + std::string{state} + R"(","fee":")" + std::string{fee} +
               R"(","updateTime":"1696760245973","reduceOnly":"false"}]})";
    };
    const auto live_body = push("live", "0", "0", "0");
    const auto live = parse_private(live_body, *storage);
    ASSERT_TRUE(live.has_value());
    map_order_push(live->orders[0], table, progress, k_ingest, events, count);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(events[0].type, OrderEventType::Accepted);
    EXPECT_EQ(events[0].qty.raw, 100);
    EXPECT_EQ(events[0].exchange_client_order_id, coid());

    const auto part_body = push("partially_filled", "4", "28000.000000000000000000", "0.100000000000000000");
    const auto part = parse_private(part_body, *storage);
    map_order_push(part->orders[0], table, progress, k_ingest, events, count);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(events[0].type, OrderEventType::Filled);
    EXPECT_EQ(events[0].qty.raw, 40);
    EXPECT_EQ(events[0].leaves_qty.raw, 60);
    EXPECT_EQ(events[0].price.raw, 280000);
    EXPECT_EQ(events[0].exec_id.view(), "28334315-40");

    const auto done_body = push("filled", "10", "28000.600000000000000000", "0.250000000000000000");
    const auto done = parse_private(done_body, *storage);
    map_order_push(done->orders[0], table, progress, k_ingest, events, count);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(events[0].qty.raw, 60);
    EXPECT_EQ(events[0].leaves_qty.raw, 0);
    EXPECT_EQ(events[0].price.raw, 280010);
    EXPECT_EQ(events[0].fee, FixedE9::from_raw(150'000'000));
    EXPECT_EQ(events[0].exec_id.view(), "28334315-100");
    EXPECT_EQ(progress.count("28334315"), 0U);

    // a replay after the order closed derives the same exec id, which the oms already holds
    map_order_push(done->orders[0], table, progress, k_ingest, events, count);
    EXPECT_EQ(events[0].exec_id.view(), "28334315-100");
}

TEST(BlofinReconciliation, ReadsPendingOrdersFillsPositionsAndTheFuturesBalance) {
    const auto table = btc_perp_table();
    const auto orders_storage = std::make_unique<OrderBuffer>();
    const auto pending_body = on_btc(fixture("rest_orders_pending.json"));
    const auto pending = parse_orders(pending_body, *orders_storage);
    ASSERT_TRUE(pending.has_value());
    ASSERT_FALSE(pending->empty());
    const auto report = map_order_report(pending->front(), table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->qty.raw, 10);
    EXPECT_EQ(report->leaves.raw, 10);
    EXPECT_EQ(report->exchange_order_id.view(), "29531103");

    const auto fills_storage = std::make_unique<FillBuffer>();
    const auto fills_body = on_btc(fixture("rest_fills_history.json"));
    const auto fills = parse_fills(fills_body, *fills_storage);
    ASSERT_TRUE(fills.has_value());
    const auto fill = map_fill(fills->front(), table);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->qty.raw, 20);
    EXPECT_EQ(fill->price.raw, 15878);
    EXPECT_EQ(fill->exec_id.view(), "7772187");

    const auto positions_storage = std::make_unique<PositionBuffer>();
    const auto positions_body = on_btc(fixture("rest_positions.json"));
    const auto positions = parse_positions(positions_body, *positions_storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 2U);
    const auto held = map_position(positions->front(), table);
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(held->net_qty.raw, 10);
    EXPECT_FALSE(map_position(positions->back(), table).has_value());
    EXPECT_EQ(map_position(WirePosition{.inst_id = "BTC-USDT", .positions = "-2.5", .average_price = "1"}, table)
                  ->net_qty.raw,
              -25);

    BalanceBuffer balance_storage{};
    const auto balance_body = fixture("rest_balance.json");
    const auto balances = parse_balances(balance_body, balance_storage);
    ASSERT_TRUE(balances.has_value());
    const auto usdt = map_balance(balances->front(), k_ingest);
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->asset, *to_asset_code("USDT"));
    EXPECT_EQ(usdt->total, FixedE9::from_raw(10'013'119'885'958'415));
}

TEST(BlofinCaps, StatesRestOrderEntryWithAPassphrase) {
    const auto& caps = exec::caps_of(VenueId::Blofin);
    EXPECT_FALSE(caps.ws_order_entry);
    EXPECT_TRUE(caps.amend_order);
    EXPECT_TRUE(caps.native_post_only);
    EXPECT_TRUE(caps.needs_passphrase);
    EXPECT_TRUE(caps.position_pull.perp);
    EXPECT_EQ(caps.user_stream, exec::UserStreamModel::AuthSubscribe);
    EXPECT_EQ(caps.max_client_order_id_len, 32);
}

} // namespace
} // namespace raikiri::venues::blofin
