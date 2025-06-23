#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bitmex/bitmex_venue.hpp>
#include <raikiri/venues/bitmex/book.hpp>
#include <raikiri/venues/bitmex/errors.hpp>
#include <raikiri/venues/bitmex/map.hpp>
#include <raikiri/venues/bitmex/rest.hpp>
#include <raikiri/venues/bitmex/wire.hpp>
#include <raikiri/venues/bitmex/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace raikiri::venues::bitmex {
namespace {

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

// Every fixture is a byte-for-byte capture; see fixtures/README.md for what each
// one came from. The trailing newline is stripped because the parsers are handed
// exactly what the socket delivers.
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// The buffers are static because they are sized for a whole venue catalogue and
// the caller's string, not the buffer, is what the returned views point into.
std::span<const WireInstrument> catalogue_rows(const std::string& body) {
    static InstrumentBuffer storage;
    const auto rows = parse_instruments(body, storage);
    EXPECT_TRUE(rows.has_value());
    return rows.has_value() ? *rows : std::span<const WireInstrument>{};
}

const WireInstrument* find_row(std::span<const WireInstrument> rows, std::string_view symbol) {
    for (const auto& row : rows) {
        if (row.symbol == symbol) {
            return &row;
        }
    }
    return nullptr;
}

std::optional<InstrumentBase> perp_base(std::string_view symbol) {
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), symbol);
    if (row == nullptr) {
        return std::nullopt;
    }
    const auto instrument = map_instrument(*row, MarketType::Perp);
    return instrument.has_value() ? std::optional{base_of(*instrument)} : std::nullopt;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// ------------------------------------------------------------ adapter ----

TEST(BitmexVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter = exec::make_venue(
        {.venue = VenueId::Bitmex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bitmex);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(BitmexVenue, OrderEntryRefusesEveryMarketThisAdapterDoesNotServe) {
    // BitMEX serves spot and the perpetuals here. Dated futures price correctly
    // in map.cpp but VenueConfig carries no switch that loads them, and options
    // are not listed at all — so both must refuse BY NAME. find() compares the
    // WHOLE id, so an id for an unloaded market would otherwise resolve to
    // nothing and blame the catalogue for a market never asked for.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Bitmex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    for (const auto market : {MarketType::Future, MarketType::Option}) {
        const auto orders = adapter->execution->open_order_reports(market);
        ASSERT_FALSE(orders.has_value()) << to_string(market);
        EXPECT_EQ(orders.error().cls, exec::ErrorClass::Terminal);
        EXPECT_FALSE(adapter->execution->fill_reports(market, UnixNanos{0}).has_value());
        EXPECT_FALSE(adapter->execution->position_reports(market).has_value());
    }

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.instrument = InstrumentId::make(VenueId::Bitmex, MarketType::Option, SymbolId{0});
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
}

TEST(BitmexVenue, PullsWithoutCredentialsAnswerAuthFailureNotTerminal) {
    // A gateway started without a key is misconfigured, not broken: the
    // reconciler retries after an operator fixes it, and halting the venue as
    // Terminal is the wrong response to a fixable state.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue(
        {.venue = VenueId::Bitmex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto started = adapter->execution->start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().cls, exec::ErrorClass::AuthFailure);

    // A pull this adapter cannot perform must SAY so. Answering with an empty
    // vector would be indistinguishable from "the venue holds nothing of ours",
    // and a reconciler reading it that way concludes we are flat when we are not.
    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);
    const auto fills = adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0});
    ASSERT_FALSE(fills.has_value());
    EXPECT_EQ(fills.error().cls, exec::ErrorClass::AuthFailure);
}

// --------------------------------------------------------------- wire ----

TEST(BitmexWire, ClassifiesEveryFrameShape) {
    const auto expect = [](std::string_view file, TableKind table, FrameAction action) {
        const auto frame = fixture(file);
        const auto info = parse_frame(frame);
        ASSERT_TRUE(info.has_value()) << file;
        EXPECT_EQ(info->table, table) << file;
        EXPECT_EQ(info->action, action) << file;
    };
    expect("ws_welcome.json", TableKind::Info, FrameAction::None);
    expect("ws_subscribe_ack.json", TableKind::SubscribeAck, FrameAction::None);
    expect("ws_error.json", TableKind::Error, FrameAction::None);
    expect("ws_quote_partial.json", TableKind::Quote, FrameAction::Partial);
    expect("ws_quote_insert.json", TableKind::Quote, FrameAction::Insert);
    expect("ws_trade_insert.json", TableKind::Trade, FrameAction::Insert);
    expect("ws_instrument_update_mark.json", TableKind::Instrument, FrameAction::Update);
    expect("ws_book_delete.json", TableKind::OrderBook, FrameAction::Delete);
}

TEST(BitmexWire, KeepaliveAnswerIsNotJson) {
    // BitMEX answers the bare text "ping" with the bare text "pong"; the frame
    // handler must reach the ignore path rather than any table.
    EXPECT_FALSE(parse_frame("pong").has_value());
    EXPECT_FALSE(parse_frame("").has_value());
}

TEST(BitmexWire, SurfacesSubscribeRefusal) {
    const auto frame = fixture("ws_error.json");
    const auto err = parse_stream_error(frame);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->status, 400);
    EXPECT_NE(err->message.find("NOSUCHSYMBOL"), std::string_view::npos);
    // 400 on a subscribe is terminal for that arg: the symbol will not appear.
    EXPECT_EQ(classify(err->status, {}), exec::ErrorClass::Terminal);
}

TEST(BitmexWire, AckNamesOneArgNotTheMessage) {
    const auto frame = fixture("ws_subscribe_ack.json");
    const auto ack = parse_subscribe_ack(frame);
    ASSERT_TRUE(ack.has_value());
    EXPECT_TRUE(ack->success);
    // The request carried four args and this is the ack for one of them.
    EXPECT_EQ(ack->topic, "quote:XBTUSD");
}

TEST(BitmexWire, NumbersKeepTheVenuesOwnSpelling) {
    // The zero-copy contract: numeric columns come back as the raw token, so
    // parse_decimal_e9 sees BitMEX's digits and no double ever touches them.
    const auto frame = fixture("ws_quote_partial.json");
    static QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[0].bid_price, "64054.7");
    EXPECT_EQ((*rows)[0].bid_qty, "80200");
    EXPECT_EQ((*rows)[0].ask_price, "64089.0");
}

TEST(BitmexWire, QuotePartialCarriesOneRowPerPool) {
    const auto frame = fixture("ws_quote_partial.json");
    static QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[0].pool, "Primary");
    EXPECT_EQ((*rows)[1].pool, "Secondary");
    // Even inside a snapshot, a pool that has no offer simply omits the columns.
    EXPECT_FALSE((*rows)[0].ask_price.empty());
    EXPECT_TRUE((*rows)[1].ask_price.empty());
    EXPECT_TRUE((*rows)[1].ask_qty.empty());
}

TEST(BitmexWire, QuoteInsertMayCarryOneSideOnly) {
    // The frame that would break a parser demanding every column: action is
    // `insert`, not `update`, and the offer side is still absent.
    const auto frame = fixture("ws_quote_insert_bid_only.json");
    static QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ((*rows)[0].bid_price, "64054.8");
    EXPECT_TRUE((*rows)[0].ask_price.empty());
}

TEST(BitmexWire, QuoteRejectsAPriceWithoutItsSize) {
    // Optional does not mean independent: pairing a fresh price with the size
    // left over from the old one is worse than dropping the frame.
    const std::string frame =
        R"({"table":"quote","action":"insert","data":[{"timestamp":"2026-08-10T20:53:31.008Z",)"
        R"("symbol":"XBTUSD","bidPrice":64054.8,"pool":"Secondary"}]})";
    static QuoteBuffer storage;
    EXPECT_FALSE(parse_quotes(frame, storage).has_value());
}

TEST(BitmexWire, QuoteRejectsARowThatMovedNothing) {
    const std::string frame =
        R"({"table":"quote","action":"insert","data":[{"timestamp":"2026-08-10T20:53:31.008Z",)"
        R"("symbol":"XBTUSD","pool":"Secondary"}]})";
    static QuoteBuffer storage;
    EXPECT_FALSE(parse_quotes(frame, storage).has_value());
}

TEST(BitmexWire, TradesAreNeverPartial) {
    const auto frame = fixture("ws_trade_partial.json");
    static TradeBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[0].price, "64088.9");
    EXPECT_EQ((*rows)[0].qty, "10000");
    // `side` is the aggressor's, stated directly — nothing is inverted.
    EXPECT_TRUE((*rows)[0].aggressor_bought);
    EXPECT_EQ((*rows)[0].trade_id, "00000000-006d-1000-0000-00364b03d4f6");
}

TEST(BitmexWire, TradeSellIsNotInverted) {
    const auto frame = fixture("ws_trade_insert.json");
    static TradeBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_FALSE((*rows)[0].aggressor_bought);
}

TEST(BitmexWire, InstrumentUpdateCarriesOnlyChangedColumns) {
    const auto frame = fixture("ws_instrument_update_mark.json");
    const auto tick = parse_instrument_tick(frame);
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->symbol, "XBTUSD");
    EXPECT_EQ(tick->mark_price, "64108.22");
    // Four of seventy-eight columns: everything funding-related is absent, and
    // absent must not read as zero anywhere downstream.
    EXPECT_TRUE(tick->funding_rate.empty());
    EXPECT_TRUE(tick->indicative_funding_rate.empty());
    EXPECT_TRUE(tick->funding_ts.empty());
    EXPECT_TRUE(tick->funding_interval.empty());
}

TEST(BitmexWire, InstrumentPartialCarriesEveryColumn) {
    const auto frame = fixture("ws_instrument_partial.json");
    const auto tick = parse_instrument_tick(frame);
    ASSERT_TRUE(tick.has_value());
    // The websocket serialiser writes what REST writes as 0.0001 in exponent
    // form; the token is preserved verbatim and decoded in the mapper.
    EXPECT_EQ(tick->funding_rate, "1.0E-4");
    EXPECT_EQ(tick->indicative_funding_rate, "1.0E-5");
    EXPECT_EQ(tick->funding_ts, "2026-08-11T04:00:00.000Z");
    EXPECT_EQ(tick->funding_interval, "2000-01-01T08:00:00.000Z");
}

TEST(BitmexWire, BookDeleteCarriesNoSize) {
    const auto frame = fixture("ws_book_delete.json");
    static LevelBuffer storage;
    const auto rows = parse_book(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ((*rows)[0].id, 233'187'800'114);
    EXPECT_FALSE((*rows)[0].bid);
    EXPECT_TRUE((*rows)[0].qty.empty());
    EXPECT_EQ((*rows)[0].price, "64091.0");
}

TEST(BitmexWire, BookSnapshotAndUpdateParse) {
    static LevelBuffer storage;
    const auto snapshot = fixture("ws_book_partial.json");
    const auto levels = parse_book(snapshot, storage);
    ASSERT_TRUE(levels.has_value());
    EXPECT_EQ(levels->size(), 50U);

    const auto update = fixture("ws_book_update.json");
    const auto changed = parse_book(update, storage);
    ASSERT_TRUE(changed.has_value());
    ASSERT_EQ(changed->size(), 2U);
    EXPECT_TRUE((*changed)[0].bid);
    EXPECT_EQ((*changed)[0].qty, "59500");
}

TEST(BitmexWire, ParsedViewsPointIntoTheCallersBuffer) {
    // The rebase contract: nothing handed out may point into the scratch buffer
    // the next frame overwrites, and nothing may come from simdjson's own
    // unescape buffer either.
    const auto frame = std::make_unique<std::string>(fixture("ws_quote_partial.json"));
    static QuoteBuffer storage;
    const auto rows = parse_quotes(*frame, storage);
    ASSERT_TRUE(rows.has_value());
    const auto* const begin = frame->data();
    const auto* const end = begin + frame->size();
    for (const auto& row : *rows) {
        EXPECT_GE(row.symbol.data(), begin);
        EXPECT_LE(row.symbol.data() + row.symbol.size(), end);
        EXPECT_GE(row.bid_price.data(), begin);
        EXPECT_LE(row.bid_price.data() + row.bid_price.size(), end);
    }
}

// ---------------------------------------------------------- timestamps ----

TEST(BitmexMap, ParsesTheFixedWidthTimestampForm) {
    constexpr UnixNanos fallback{7};
    EXPECT_EQ(iso8601_to_unix("1970-01-01T00:00:00.000Z", fallback).ns, 0ULL);
    EXPECT_EQ(iso8601_to_unix("2026-08-10T20:53:31.008Z", fallback).ns,
              1'786'395'211'008'000'000ULL);
    EXPECT_EQ(iso8601_to_unix("2026-12-25T12:00:00.000Z", fallback).ns,
              1'798'200'000'000'000'000ULL);
}

TEST(BitmexMap, RefusesAnythingButTheFixedWidthForm) {
    constexpr UnixNanos fallback{7};
    EXPECT_EQ(iso8601_to_unix("2026-08-10T20:53:31Z", fallback).ns, fallback.ns);     // no millis
    EXPECT_EQ(iso8601_to_unix("2026-13-10T20:53:31.008Z", fallback).ns, fallback.ns); // month 13
    EXPECT_EQ(iso8601_to_unix("1786395211008", fallback).ns, fallback.ns);            // epoch ms
    EXPECT_EQ(iso8601_to_unix("", fallback).ns, fallback.ns);
}

TEST(BitmexMap, TimestampsAreNotScaledDecimals) {
    // A timestamp routed through parse_decimal_e9 would be multiplied by 1e9 and
    // wrap int64 silently. The 2026 instant below is 1.79e18 ns — within range
    // only because it never went near that path.
    const auto ns = iso8601_to_unix("2026-08-11T04:00:00.000Z", UnixNanos{0}).ns;
    EXPECT_EQ(ns, 1'786'420'800'000'000'000ULL);
    EXPECT_LT(ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

TEST(BitmexMap, FundingIntervalIsADurationWrittenAsATimestamp) {
    // "2000-01-01T08:00:00.000Z" means eight hours; the date half is noise.
    EXPECT_EQ(funding_interval_seconds("2000-01-01T08:00:00.000Z"), 28'800U);
    EXPECT_EQ(funding_interval_seconds("2000-01-01T04:00:00.000Z"), 14'400U);
    EXPECT_EQ(funding_interval_seconds("2000-01-01T00:01:00.000Z"), 60U);
    EXPECT_EQ(funding_interval_seconds(""), 0U);
}

// ----------------------------------------------------------- catalogue ----

TEST(BitmexMap, InverseContractValueIsQuoteCurrencyPerContract) {
    // THE point of this adapter. XBTUSD is quoted in USD and settled in BTC, and
    // one contract is worth exactly one USD — so contract_value is 1 (USD per
    // contract), NOT 1/price (BTC per contract). The BTC side is what floats.
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTUSD");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->inverse);
    // BitMEX states the direction twice; the negative multiplier is the second
    // statement and it is cross-checked, never stored.
    EXPECT_EQ(row->multiplier, "-100000000");

    const auto instrument = map_instrument(*row, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    const auto& base = base_of(*instrument);
    EXPECT_EQ(base.contract_value.raw, k_e9); // 1 USD
    EXPECT_GT(base.contract_value.raw, 0);    // never the raw sign
    EXPECT_EQ(base.tick_size.raw, 100'000'000);
    EXPECT_EQ(base.lot_size.raw, 100 * k_e9);
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 28'800U);
    EXPECT_TRUE(validate(base));
}

TEST(BitmexMap, LinearContractValueIsBaseCurrencyPerContract) {
    // The same venue, the other direction: XBTUSDT settles in USDT and one
    // contract is 1e-6 BTC. Reading these two rows the same way is the mistake
    // the inverse comment in map.cpp exists to prevent.
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTUSDT");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->inverse);
    EXPECT_EQ(row->multiplier, "1");

    const auto instrument = map_instrument(*row, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 1'000); // 1e-6 XBT
}

TEST(BitmexMap, SpotContractValueIsBaseUnitsPerSizeUnit) {
    // Spot sizes are integer minor units of the base currency, so one size unit
    // is 1 / underlyingToPositionMultiplier.
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "SOL_USDT");
    ASSERT_NE(row, nullptr);
    const auto instrument = map_instrument(*row, MarketType::Spot);
    ASSERT_TRUE(instrument.has_value());
    ASSERT_TRUE(std::holds_alternative<SpotInstrument>(*instrument));
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 1); // 1e-9 SOL
}

TEST(BitmexMap, RefusesRowsThatWouldMispriceSilently) {
    const auto body = fixture("rest_instruments.json");
    const auto rows = catalogue_rows(body);

    // Settled: /instrument/active returns it anyway, and every numeric column
    // still looks perfectly usable.
    const auto* settled = find_row(rows, "XRPU26");
    ASSERT_NE(settled, nullptr);
    EXPECT_EQ(settled->state, "Settled");
    EXPECT_FALSE(map_instrument(*settled, MarketType::Future).has_value());

    // Quanto: no single-currency contract size exists for it.
    const auto* quanto = find_row(rows, "ETHUSD");
    ASSERT_NE(quanto, nullptr);
    EXPECT_TRUE(quanto->quanto);
    EXPECT_FALSE(map_instrument(*quanto, MarketType::Perp).has_value());

    // A dated future handed back for a perp request is a position bug.
    const auto* dated = find_row(rows, "XBTZ26");
    ASSERT_NE(dated, nullptr);
    EXPECT_FALSE(map_instrument(*dated, MarketType::Perp).has_value());
}

TEST(BitmexMap, AnExpiringPerpetualIsStillAPerpetual) {
    // BitMEX schedules a delisting by putting an `expiry` on a PERPETUAL: this
    // row is Open, funds every 8h, and stops trading at noon. Twelve rows grew
    // one overnight, so treating expiry as "this is a dated future" silently
    // dropped a fifth of the live catalogue.
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTEUR");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->expiry.empty());
    EXPECT_FALSE(row->funding_interval.empty());

    const auto instrument = map_instrument(*row, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    // Inverse against EUR rather than USD: contract_value is one unit of the
    // QUOTE currency whatever that currency happens to be.
    EXPECT_EQ(base_of(*instrument).contract_value.raw, k_e9);
    // Funding is what separates the two, so the same row must not pass as one.
    EXPECT_FALSE(map_instrument(*row, MarketType::Future).has_value());
}

TEST(BitmexMap, DatedFutureCarriesItsExpiry) {
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTZ26");
    ASSERT_NE(row, nullptr);
    const auto instrument = map_instrument(*row, MarketType::Future);
    ASSERT_TRUE(instrument.has_value());
    ASSERT_TRUE(std::holds_alternative<FutureInstrument>(*instrument));
    EXPECT_EQ(std::get<FutureInstrument>(*instrument).expiry.ns, 1'798'200'000'000'000'000ULL);
    // Dated or not, an inverse contract is still one USD per contract.
    EXPECT_EQ(base_of(*instrument).contract_value.raw, k_e9);
}

TEST(BitmexMap, CatalogueSplitsCleanlyByMarket) {
    const auto body = fixture("rest_instruments.json");
    const auto rows = catalogue_rows(body);
    ASSERT_EQ(rows.size(), 7U);
    const auto count = [rows](MarketType market) {
        std::size_t n = 0;
        for (const auto& row : rows) {
            n += map_instrument(row, market).has_value() ? 1 : 0;
        }
        return n;
    };
    EXPECT_EQ(count(MarketType::Perp), 3U);   // XBTUSD, XBTUSDT, XBTEUR
    EXPECT_EQ(count(MarketType::Spot), 1U);   // SOL_USDT
    EXPECT_EQ(count(MarketType::Future), 1U); // XBTZ26; XRPU26 is settled
    EXPECT_EQ(count(MarketType::Option), 0U);
}

// -------------------------------------------------------------- merge ----

TEST(BitmexQuoteBook, SumsPoolsAtTheBestPrice) {
    // BitMEX publishes a top of book per liquidity pool. The venue's own
    // Aggregated book reports 2123700 at 64054.7, which is Primary's 80200 plus
    // Secondary's 2043500 — so the pools sum rather than compete.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    const auto frame = fixture("ws_quote_partial.json");
    static QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);

    QuoteBook book;
    const auto primary = book.apply((*rows)[0], *base, k_ingest);
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->bid_qty.raw, 802); // 80200 / lot 100

    const auto merged = book.apply((*rows)[1], *base, k_ingest);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->bid.raw, 640'547); // 64054.7 / tick 0.1
    EXPECT_EQ(merged->ask.raw, 640'890);
    EXPECT_EQ(merged->bid_qty.raw, 21'237); // (80200 + 2043500) / 100
    EXPECT_EQ(merged->ask_qty.raw, 199);    // Primary alone quotes an offer
    EXPECT_EQ(merged->ts_event.ns, 1'786'395'210'148'000'000ULL);
    EXPECT_EQ(merged->ts_init.ns, k_ingest.ns);
}

TEST(BitmexQuoteBook, WithholdsAHalfBook) {
    // A fresh subscription is one-sided until the pool that owns the other side
    // speaks; publishing then would put an undefined offer in the seqlock cell.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    const auto frame = fixture("ws_quote_insert_bid_only.json");
    static QuoteBuffer storage;
    const auto rows = parse_quotes(frame, storage);
    ASSERT_TRUE(rows.has_value());

    QuoteBook book;
    EXPECT_FALSE(book.apply((*rows)[0], *base, k_ingest).has_value());
}

TEST(BitmexQuoteBook, OneSidedUpdateLeavesTheOtherSideStanding) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    static QuoteBuffer storage;

    QuoteBook book;
    const auto snapshot = fixture("ws_quote_partial.json");
    const auto rows = parse_quotes(snapshot, storage);
    ASSERT_TRUE(rows.has_value());
    (void)book.apply((*rows)[0], *base, k_ingest);
    (void)book.apply((*rows)[1], *base, k_ingest);

    // Secondary improves its bid and says nothing about its offer.
    const auto insert = fixture("ws_quote_insert_bid_only.json");
    const auto one_sided = parse_quotes(insert, storage);
    ASSERT_TRUE(one_sided.has_value());
    const auto after = book.apply((*one_sided)[0], *base, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->bid.raw, 640'548);   // 64054.8, now the sole best bid
    EXPECT_EQ(after->bid_qty.raw, 1'498); // Primary no longer ties, so it drops out
    EXPECT_EQ(after->ask.raw, 640'890);   // untouched by a bid-only frame
    EXPECT_EQ(after->ask_qty.raw, 199);
}

TEST(BitmexInstrumentState, MarkOnlyUpdateIsNotAFundingEvent) {
    // The shm funding cell is overwritten whole, so a FundingUpdate assembled
    // from this frame would erase a real rate with a plausible zero.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    const auto frame = fixture("ws_instrument_update_mark.json");
    const auto tick = parse_instrument_tick(frame);
    ASSERT_TRUE(tick.has_value());

    InstrumentState state;
    EXPECT_FALSE(state.apply(*tick, *base, k_ingest).has_value());
}

TEST(BitmexInstrumentState, SnapshotPublishesTheWholeFundingPicture) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    const auto frame = fixture("ws_instrument_partial.json");
    const auto tick = parse_instrument_tick(frame);
    ASSERT_TRUE(tick.has_value());

    InstrumentState state;
    const auto funding = state.apply(*tick, *base, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->instrument.packed, base->id.packed);
    EXPECT_EQ(funding->rate.raw, 100'000);          // 1.0E-4 decoded exactly
    EXPECT_EQ(funding->predicted_rate.raw, 10'000); // 1.0E-5
    EXPECT_EQ(funding->interval_s, 28'800U);
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'420'800'000'000'000ULL);
}

TEST(BitmexInstrumentState, PrimedStateStillIgnoresANonFundingUpdate) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    InstrumentState state;

    const auto snapshot = fixture("ws_instrument_partial.json");
    const auto primed = parse_instrument_tick(snapshot);
    ASSERT_TRUE(primed.has_value());
    ASSERT_TRUE(state.apply(*primed, *base, k_ingest).has_value());

    // Rate is known now, but this frame did not move it, and republishing on
    // every mark tick would make a funding stream out of a price stream.
    const auto quote_only = fixture("ws_instrument_update_quote.json");
    const auto tick = parse_instrument_tick(quote_only);
    ASSERT_TRUE(tick.has_value());
    EXPECT_FALSE(state.apply(*tick, *base, k_ingest).has_value());
}

TEST(BitmexMap, TradeConvertsAgainstTheInstrumentsOwnSteps) {
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTUSD");
    ASSERT_NE(row, nullptr);
    const auto instrument = map_instrument(*row, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    exec::InstrumentTable table;
    ASSERT_TRUE(table.add(*instrument).has_value());

    const auto frame = fixture("ws_trade_partial.json");
    static TradeBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    const auto trade = map_trade((*rows)[0], table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->price.raw, 640'889); // 64088.9 / 0.1
    EXPECT_EQ(trade->qty.raw, 100);       // 10000 contracts / lot 100
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->ts_event.ns, 1'786'395'121'216'000'000ULL);
}

TEST(BitmexMap, DropsAPrintForAnUnknownSymbol) {
    const exec::InstrumentTable empty;
    const auto frame = fixture("ws_trade_partial.json");
    static TradeBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_FALSE(map_trade((*rows)[0], empty, k_ingest).has_value());
}

TEST(BitmexMap, DropsAPriceOffTheTickGrid) {
    // XBTUSD ticks at 0.1 but BitMEX marks to 0.01, so an off-grid value is a
    // real possibility; the rule is drop, never round.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    exec::InstrumentTable table;
    const auto body = fixture("rest_instruments.json");
    const auto* row = find_row(catalogue_rows(body), "XBTUSD");
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(table.add(*map_instrument(*row, MarketType::Perp)).has_value());

    const std::string frame =
        R"({"table":"trade","action":"insert","data":[{"timestamp":"2026-08-10T21:01:38.443Z",)"
        R"("symbol":"XBTUSD","side":"Sell","size":2500,"price":64022.15,"trdMatchID":"x"}]})";
    static TradeBuffer storage;
    const auto rows = parse_trades(frame, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_FALSE(map_trade((*rows)[0], table, k_ingest).has_value());
}

// --------------------------------------------------------------- book ----

// The parsed views point into the frame text and into the level buffer, so
// both have to outlive the wire span that came out of them.
struct BookFrame {
    std::string text;
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

std::span<const WireLevel> rows_of(BookFrame& frame, std::string_view name) {
    frame.text = fixture(name);
    const auto rows = parse_book(frame.text, *frame.storage);
    EXPECT_TRUE(rows.has_value());
    return rows.has_value() ? *rows : std::span<const WireLevel>{};
}

Book book_for(const InstrumentBase& base) {
    return Book{base.id, base.tick_size, base.lot_size};
}

// Bids descend, asks ascend, the two never cross and every quantity is real.
// A book that fails this is not a book, and publishing it would price every
// strategy reading shared memory.
void expect_coherent(const BookSnapshot& out) {
    ASSERT_GT(out.bid_count, 0U);
    ASSERT_GT(out.ask_count, 0U);
    EXPECT_LE(out.bid_count, k_book_levels);
    EXPECT_LE(out.ask_count, k_book_levels);
    EXPECT_LT(out.bids[0].price.raw, out.asks[0].price.raw);
    for (std::uint32_t i = 1; i < out.bid_count; ++i) {
        EXPECT_LT(out.bids[i].price.raw, out.bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].price.raw, out.asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < out.bid_count; ++i) {
        EXPECT_GT(out.bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < out.ask_count; ++i) {
        EXPECT_GT(out.asks[i].qty.raw, 0) << i;
    }
}

TEST(BitmexBook, RowsBeforeAnyPartialPublishNothing) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame update;
    EXPECT_EQ(book.apply(FrameAction::Update, rows_of(update, "ws_book_update.json")),
              BookResult::Ignored);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(BitmexBook, PartialThenUpdateAndDeleteMakeABookSharedMemoryCanTake) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame partial;
    BookFrame update;
    BookFrame remove;

    const auto rows = rows_of(partial, "ws_book_partial.json");
    ASSERT_EQ(rows.size(), 50U);
    EXPECT_EQ(book.apply(FrameAction::Partial, rows), BookResult::Resynced);

    const auto out = book.snapshot(iso8601_to_unix(rows.front().ts, k_ingest), k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // BitMEX maintains the 25-level window itself, so the book holds exactly
    // what the venue keeps — a quarter of what shm could carry, and nothing is
    // invented to fill the rest.
    EXPECT_EQ(out->bid_count, 25U);
    EXPECT_EQ(out->ask_count, 25U);
    EXPECT_EQ(out->bids[0].price.raw, 640'547); // 64054.7 over the 0.1 tick
    EXPECT_EQ(out->asks[0].price.raw, 640'890);
    EXPECT_EQ(out->bids[0].qty.raw, 21'237); // 2123700 over the 100 lot
    EXPECT_EQ(out->ts_event.ns, 1'786'395'210'303'000'000ULL);
    // This table has no sequence of any kind, so there is nothing honest to
    // put here — an invented number would read as one BitMEX stood behind.
    EXPECT_EQ(out->venue_seq, 0ULL);

    EXPECT_EQ(book.apply(FrameAction::Update, rows_of(update, "ws_book_update.json")),
              BookResult::Applied);
    const auto amended = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(amended.has_value());
    expect_coherent(*amended);
    // 64000.1 at 59500 contracts, the first of the two rows the update carried.
    const auto at = std::find_if(amended->bids.begin(),
                                 amended->bids.begin() + amended->bid_count,
                                 [](const BookLevel& level) { return level.price.raw == 640'001; });
    ASSERT_NE(at, amended->bids.begin() + amended->bid_count);
    EXPECT_EQ(at->qty.raw, 595);

    EXPECT_EQ(book.apply(FrameAction::Delete, rows_of(remove, "ws_book_delete.json")),
              BookResult::Applied);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    expect_coherent(*after);
    EXPECT_EQ(after->ask_count, 24U) << "the delete removed a level, it did not zero one";
    const auto gone = std::find_if(
        after->asks.begin(), after->asks.begin() + after->ask_count, [](const BookLevel& level) {
            return level.price.raw == 640'910; // 64091.0
        });
    EXPECT_EQ(gone, after->asks.begin() + after->ask_count);
}

TEST(BitmexBook, ADeleteIsFoundByItsIdAndNotByItsPrice) {
    // The whole reason this book is keyed by id: BitMEX declares the row key
    // as ["symbol","id","side"], and the price on a delete row is a courtesy
    // the venue is free to stop paying.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame partial;
    BookFrame remove;
    EXPECT_EQ(book.apply(FrameAction::Partial, rows_of(partial, "ws_book_partial.json")),
              BookResult::Resynced);

    auto rows = rows_of(remove, "ws_book_delete.json");
    ASSERT_EQ(rows.size(), 1U);
    WireLevel lying = rows.front();
    lying.price = "64200.1"; // a real level of the book, but not this id's

    const std::array<WireLevel, 1> one{lying};
    EXPECT_EQ(book.apply(FrameAction::Delete, one), BookResult::Applied);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->ask_count, 24U);
    // The id's level went; the price the row named is untouched.
    const auto gone =
        std::find_if(after->asks.begin(),
                     after->asks.begin() + after->ask_count,
                     [](const BookLevel& level) { return level.price.raw == 640'910; });
    EXPECT_EQ(gone, after->asks.begin() + after->ask_count);
    EXPECT_EQ(after->asks[after->ask_count - 1].price.raw, 642'001);
    EXPECT_EQ(book.stale_deletes(), 0U);
}

TEST(BitmexBook, AnInsertAddsALevelUnderItsOwnId) {
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame partial;
    EXPECT_EQ(book.apply(FrameAction::Partial, rows_of(partial, "ws_book_partial.json")),
              BookResult::Resynced);

    // A new best bid inside the spread, which is where BitMEX's inserts land.
    const std::array<WireLevel, 1> arrival{WireLevel{.symbol = "XBTUSD",
                                                     .id = 233187800999,
                                                     .bid = true,
                                                     .price = "64060.0",
                                                     .qty = "1000",
                                                     .ts = "2026-08-10T20:53:30.500Z"}};
    EXPECT_EQ(book.apply(FrameAction::Insert, arrival), BookResult::Applied);
    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    EXPECT_EQ(out->bid_count, 26U) << "BitMEX pairs this with a delete; the book does not prune";
    EXPECT_EQ(out->bids[0].price.raw, 640'600);
    EXPECT_EQ(out->bids[0].qty.raw, 10);
}

TEST(BitmexBook, AnUpdateForAnIdWeDoNotHoldDropsTheBook) {
    // This table has no sequence, so a lost frame is invisible everywhere
    // except here: an update can only name a row BitMEX believes we hold.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame partial;
    BookFrame update;
    EXPECT_EQ(book.apply(FrameAction::Partial, rows_of(partial, "ws_book_partial.json")),
              BookResult::Resynced);

    auto rows = rows_of(update, "ws_book_update.json");
    WireLevel orphan = rows.front();
    orphan.id += 1; // the insert that created this id never reached us

    const std::array<WireLevel, 1> one{orphan};
    EXPECT_EQ(book.apply(FrameAction::Update, one), BookResult::Diverged);
    EXPECT_EQ(book.divergences(), 1U);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(BitmexBook, ALevelOffTheStepInvalidatesTheWholeBook) {
    // The rule that separates a delta venue from a snapshot venue. Dropping
    // one level here would leave a book that disagrees with BitMEX's until the
    // next reconnect, with nothing downstream able to see the difference.
    const auto base = perp_base("XBTUSD");
    ASSERT_TRUE(base.has_value());
    auto book = book_for(*base);
    BookFrame partial;
    EXPECT_EQ(book.apply(FrameAction::Partial, rows_of(partial, "ws_book_partial.json")),
              BookResult::Resynced);

    const std::array<WireLevel, 1> off_tick{
        WireLevel{.symbol = "XBTUSD",
                  .id = 233187800998,
                  .bid = true,
                  .price = "64060.05", // finer than the 0.1 tick
                  .qty = "1000",
                  .ts = "2026-08-10T20:53:30.500Z"}};
    EXPECT_EQ(book.apply(FrameAction::Insert, off_tick), BookResult::BadLevel);
    EXPECT_FALSE(book.synced());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());

    // A size that is not a whole lot of 100 contracts is refused the same way.
    auto again = book_for(*base);
    BookFrame again_partial;
    EXPECT_EQ(again.apply(FrameAction::Partial, rows_of(again_partial, "ws_book_partial.json")),
              BookResult::Resynced);
    const std::array<WireLevel, 1> sub_lot{WireLevel{.symbol = "XBTUSD",
                                                     .id = 233187800997,
                                                     .bid = true,
                                                     .price = "64060.0",
                                                     .qty = "150",
                                                     .ts = "2026-08-10T20:53:30.500Z"}};
    EXPECT_EQ(again.apply(FrameAction::Insert, sub_lot), BookResult::BadLevel);
    EXPECT_FALSE(again.snapshot(k_ingest, k_ingest).has_value());
}

// ------------------------------------------------------------- errors ----

TEST(BitmexErrors, ClassifiesTheStatusVocabulary) {
    EXPECT_EQ(classify(k_too_many_requests, {}), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_ok, "RateLimitError"), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_unauthorized, "HTTPError"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_forbidden, "HTTPError"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_service_unavailable, {}), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_gateway_timeout, {}), exec::ErrorClass::Retryable);
    // Terminal is the default, including for statuses BitMEX has never sent.
    EXPECT_EQ(classify(k_bad_request, "ValidationError"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_not_found, "HTTPError"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(418, {}), exec::ErrorClass::Terminal);
}

TEST(BitmexErrors, ReadsTheOnlyErrorBodyShape) {
    const auto validation = fixture("rest_error_validation.json");
    const auto parsed = parse_rest_error(validation);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->name, "ValidationError");
    EXPECT_NE(parsed->message.find("Maximum result count"), std::string_view::npos);

    // 403 and 404 share the name "HTTPError", which is why the status leads and
    // the name only refines.
    const auto forbidden = fixture("rest_error_forbidden.json");
    const auto denied = parse_rest_error(forbidden);
    ASSERT_TRUE(denied.has_value());
    EXPECT_EQ(denied->name, "HTTPError");
    const auto missing = fixture("rest_error_not_found.json");
    const auto absent = parse_rest_error(missing);
    ASSERT_TRUE(absent.has_value());
    EXPECT_EQ(absent->name, "HTTPError");
    EXPECT_NE(classify(k_forbidden, denied->name), classify(k_not_found, absent->name));
}

TEST(BitmexErrors, CatalogueBodyIsNotAnErrorBody) {
    // Success is a bare array; there is no envelope, so the error reader must
    // fail on a good body rather than report a zero-code success.
    const auto body = fixture("rest_instruments.json");
    EXPECT_FALSE(parse_rest_error(body).has_value());
}

// ---------------------------------------------------------- protocol ----

TEST(BitmexWs, MapsSubscriptionKindsToTables) {
    EXPECT_EQ(topic_for(exec::SubKind::Bbo), "quote");
    EXPECT_EQ(topic_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(topic_for(exec::SubKind::Depth), "orderBookL2_25");
    // One table carries both, unlike okx.
    EXPECT_EQ(topic_for(exec::SubKind::Funding), "instrument");
    EXPECT_EQ(topic_for(exec::SubKind::MarkPrice), "instrument");
    EXPECT_TRUE(topic_for(exec::SubKind::Kline).empty());
}

TEST(BitmexWs, BuildsTheSubscribeFrame) {
    Protocol protocol;
    const std::array<Subscription, 2> args{Subscription{.table = "quote", .symbol = "XBTUSD"},
                                           Subscription{.table = "trade", .symbol = "XBTUSD"}};
    const auto frame = protocol.subscribe_frame(args);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text, R"({"op":"subscribe","args":["quote:XBTUSD","trade:XBTUSD"]})");
}

TEST(BitmexWs, RefusesAnUnscopedSubscription) {
    // "quote" with no symbol is valid BitMEX and streams every contract on the
    // venue — an error is safer than a subscription that succeeds too well.
    Protocol protocol;
    const std::array<Subscription, 1> unscoped{Subscription{.table = "quote", .symbol = {}}};
    const auto frame = protocol.subscribe_frame(unscoped);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(BitmexRest, BuildsTheReadTargets) {
    RequestBuilder builder;
    EXPECT_EQ(builder.instruments().target, "/api/v1/instrument/active");
    // NOT /orderBookL2_25: that is a websocket table name and 404s over REST.
    EXPECT_EQ(builder.book("XBTUSD", 25).target, "/api/v1/orderBook/L2?symbol=XBTUSD&depth=25");
    EXPECT_EQ(builder.trades("XBTUSD", 2).target,
              "/api/v1/trade?symbol=XBTUSD&reverse=true&count=2");
    EXPECT_EQ(rest_host(false), "www.bitmex.com");
    EXPECT_EQ(rest_host(true), "testnet.bitmex.com");
    EXPECT_EQ(stream_host(false), "ws.bitmex.com");
    EXPECT_EQ(stream_path(), "/realtime");
}


// ------------------------------------------------------------- exec ----

// BitMEX's OWN published authentication example secret. The three digests
// asserted below are the ones its documentation prints for it, so this test
// checks the implementation against the venue rather than against itself.
constexpr std::string_view k_doc_secret = "chNOOS4KvNXR_Xq4k4c9qsfoKWvnDecLATCRlcBwyKDYnWgO";

TEST(BitmexSign, MatchesTheVenuesOwnPublishedVectors) {
    std::array<char, net::k_hex_bytes> out{};
    // verb + path + expires + body, bare concatenation, hex. A GET has no body
    // and still signs the empty string after the expiry.
    EXPECT_EQ(net::hmac_sha256_hex(k_doc_secret, "GET/api/v1/instrument1518064236", out),
              "c7682d435d0cfe87c16098df34ef2eb5a549d4c5a3c2b1f0f77b8af73423bf00");
    // The path INCLUDES the query string, percent-encoded exactly as it travels.
    EXPECT_EQ(net::hmac_sha256_hex(
                  k_doc_secret,
                  "GET/api/v1/instrument?filter=%7B%22symbol%22%3A+%22XBTM15%22%7D1518064237",
                  out),
              "e2f422547eecb5b3cb29ade2127e21b858b235b386bfa45e1c1756eb3383919f");
    // A POST appends the body verbatim — the bytes that are sent, not a
    // re-serialisation of them.
    EXPECT_EQ(net::hmac_sha256_hex(k_doc_secret,
                                   R"(POST/api/v1/order1518064238{"symbol":"XBTM15",)"
                                   R"("price":219.0,"clOrdID":"mm_bitmex_1a/oemUeQ4CAJZgP3fjHsA",)"
                                   R"("orderQty":98})",
                                   out),
              "1749cd2ccae4aa49048ae09f0b95110cee706e0944e6a14ad0b3a8cb45bd336b");
}

exec::InstrumentTable loaded_table() {
    const auto body = fixture("rest_instruments.json");
    const auto rows = catalogue_rows(body);
    exec::InstrumentTable table;
    // Both markets in one table, the way a gateway with enable_spot and
    // enable_perp loads them: the pulls answer for one market at a time and the
    // filtering is what keeps them apart.
    for (const auto market : {MarketType::Perp, MarketType::Spot}) {
        for (const auto& row : rows) {
            if (auto instrument = map_instrument(row, market); instrument.has_value()) {
                EXPECT_TRUE(table.add(*instrument).has_value());
            }
        }
    }
    return table;
}

InstrumentId id_of(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr) << symbol;
    return instrument == nullptr ? InstrumentId{} : base_of(*instrument).id;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key("LAqUlngMIQkIUjXMUreyu3qn");
    ASSERT_TRUE(signer.secret().assign(k_doc_secret).has_value());
}

// XBTUSD: tick 0.1, lot 100 contracts. 640005 ticks is 64000.5, one lot is 100
// contracts of 1 USD each.
OrderCmd limit_buy(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = id_of(table, "XBTUSD");
    cmd.client_order_id = ClientOrderId::make(7, 11);
    cmd.price = PriceTicks::from_raw(640'005);
    cmd.qty = QtyLots::from_raw(1);
    return cmd;
}

// k_expiry_window_s ahead of this is 1518064236, the venue's own example.
constexpr UnixNanos k_now{(1'518'064'236ULL - SignedRequestBuilder::k_expiry_window_s) *
                          1'000'000'000ULL};

TEST(BitmexRest, ASignedPostSignsVerbPathExpiresAndTheBodyItSends) {
    const auto table = loaded_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(table), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/api/v1/order");
    EXPECT_EQ(request->body,
              R"({"symbol":"XBTUSD","side":"Buy","orderQty":100,"ordType":"Limit",)"
              R"("price":64000.5,"timeInForce":"GoodTillCancel"})");
    EXPECT_EQ(builder.last_sign_message(),
              std::string{"POST/api/v1/order1518064236"} + std::string{request->body});

    ASSERT_EQ(request->headers.size(), 3U);
    EXPECT_EQ(request->headers[0].name, "api-key");
    EXPECT_EQ(request->headers[1].name, "api-expires");
    // An absolute DEADLINE, not the request time every other venue here sends.
    EXPECT_EQ(request->headers[1].value, "1518064236");
    EXPECT_EQ(request->headers[2].name, "api-signature");
    std::array<char, net::k_hex_bytes> expected{};
    EXPECT_EQ(request->headers[2].value,
              net::hmac_sha256_hex(k_doc_secret, builder.last_sign_message(), expected));
}

TEST(BitmexRest, OrderTypeTimeInForceAndExecInstAreThreeSeparateFields) {
    const auto table = loaded_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table);
    cmd.flags = OrderFlags::PostOnly | OrderFlags::ReduceOnly;
    cmd.tif = TimeInForce::Ioc;
    const auto both = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(both.has_value());
    // execInst is a comma-separated LIST; sending only one of the two flags
    // would silently drop half of what the strategy asked for.
    EXPECT_NE(both->body.find(R"("timeInForce":"ImmediateOrCancel")"), std::string_view::npos)
        << both->body;
    EXPECT_NE(both->body.find(R"("execInst":"ParticipateDoNotInitiate,ReduceOnly")"),
              std::string_view::npos)
        << both->body;

    // A market order carries NO price: BitMEX defaults ordType from whether one
    // is present, so a price on a market order changes which order was placed.
    cmd.order_type = OrderType::Market;
    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Gtc;
    const auto market = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(market.has_value());
    EXPECT_EQ(market->body,
              R"({"symbol":"XBTUSD","side":"Buy","orderQty":100,"ordType":"Market",)"
              R"("timeInForce":"ImmediateOrCancel"})")
        << market->body;

    // A quantity that is not a whole number of the venue's own units cannot be
    // sent, and must not be rounded into one that can.
    cmd = limit_buy(table);
    cmd.qty = QtyLots::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::BadQuantity);
}

TEST(BitmexRest, CancelAndAmendCarryABodyAndTheirBodyIsSigned) {
    const auto table = loaded_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table);
    cmd.kind = CmdKind::Cancel;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("b5c5a2d4-1f04-4a3f-b0f3-9e5d5a4f9a11");
    const auto cancel = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->method, net::HttpMethod::Delete);
    EXPECT_EQ(cancel->target, "/api/v1/order");
    // A DELETE carries a body here, and that body is part of the digest.
    EXPECT_EQ(cancel->body, R"({"orderID":"b5c5a2d4-1f04-4a3f-b0f3-9e5d5a4f9a11"})");
    EXPECT_EQ(builder.last_sign_message(),
              std::string{"DELETE/api/v1/order1518064236"} + std::string{cancel->body});

    cmd.kind = CmdKind::Amend;
    const auto amend = builder.amend_order(cmd, table, k_now);
    ASSERT_TRUE(amend.has_value());
    EXPECT_EQ(amend->method, net::HttpMethod::Put);
    EXPECT_EQ(amend->body,
              R"({"orderID":"b5c5a2d4-1f04-4a3f-b0f3-9e5d5a4f9a11","orderQty":100,)"
              R"("price":64000.5})");

    // `origClOrdID`, not `clOrdID`: the latter would RENAME the order.
    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");
    const auto by_client = builder.amend_order(cmd, table, k_now);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_NE(by_client->body.find(R"("origClOrdID":"01J9ZQ8V3T4W5X6Y7Z8A9B0C1D")"),
              std::string_view::npos)
        << by_client->body;

    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
}

TEST(BitmexRest, GetsSignThePercentEncodedQueryTheyActuallySend) {
    const auto table = loaded_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto open = builder.open_orders(0, k_now);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open->method, net::HttpMethod::Get);
    // The filter is JSON: braces, quotes and a colon, none of which may travel
    // raw in a query — and the ENCODED form is what the signature covers.
    EXPECT_EQ(open->target,
              "/api/v1/order?filter=%7B%22open%22%3Atrue%7D&count=500&reverse=true&start=0");
    EXPECT_TRUE(open->body.empty());
    EXPECT_EQ(builder.last_sign_message(),
              std::string{"GET"} + std::string{open->target} + "1518064236");

    const auto query = builder.query_order(
        *ExchangeClientOrderId::from("01J9ZQ8V3T4W5X6Y7Z8A9B0C1D"), k_now);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query->target,
              "/api/v1/order?filter=%7B%22clOrdID%22%3A%2201J9ZQ8V3T4W5X6Y7Z8A9B0C1D%22%7D"
              "&count=1&reverse=true");

    const auto positions = builder.positions(k_now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_EQ(positions->target, "/api/v1/position?count=500");

    const auto history = builder.fills(UnixNanos{1'770'000'000'000'000'000ULL}, 500, k_now);
    ASSERT_TRUE(history.has_value());
    EXPECT_EQ(history->target,
              "/api/v1/execution/tradeHistory?count=500&reverse=true&start=500"
              "&startTime=2026-02-02T02%3A40%3A00.000Z");

    // A zero floor asks for the newest page and names no window at all.
    const auto newest = builder.fills(UnixNanos{0}, 0, k_now);
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(newest->target,
              "/api/v1/execution/tradeHistory?count=500&reverse=true&start=0");
}

TEST(BitmexRest, ABatchCancelIsOneRequestNamingManyVenueIds) {
    const auto table = loaded_table();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    std::array<OrderCmd, 2> cmds{limit_buy(table), limit_buy(table)};
    cmds[0].target_exchange_order_id = *ExchangeOrderId::from("aaaaaaaa-0000-0000-0000-0000000000");
    cmds[1].target_exchange_order_id = *ExchangeOrderId::from("bbbbbbbb-0000-0000-0000-0000000000");
    const auto batch = builder.cancel_batch(cmds, table, k_now);
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->method, net::HttpMethod::Delete);
    EXPECT_EQ(batch->body,
              R"({"orderID":["aaaaaaaa-0000-0000-0000-0000000000",)"
              R"("bbbbbbbb-0000-0000-0000-0000000000"]})");

    // The array form takes venue ids only, so a batch is all-or-nothing rather
    // than silently dropping the ones we cannot name.
    cmds[1].target_exchange_order_id = {};
    EXPECT_EQ(builder.cancel_batch(cmds, table, k_now).error(), BuildError::NoOrderTarget);
    EXPECT_EQ(builder.cancel_batch({}, table, k_now).error(), BuildError::EmptyBatch);
}

TEST(BitmexRest, MarketRefusalIsNamedBeforeTheInstrumentTableIsEverRead) {
    // The table here is EMPTY on purpose: the refusal must not depend on it.
    const exec::InstrumentTable empty;
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    for (const auto [market, expected] :
         {std::pair{MarketType::Future, BuildError::DatedFuturesUnsupported},
          std::pair{MarketType::Option, BuildError::OptionsUnsupported}}) {
        OrderCmd cmd{};
        cmd.instrument = InstrumentId::make(VenueId::Bitmex, market, SymbolId{3});
        EXPECT_EQ(builder.create_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(builder.cancel_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(builder.amend_order(cmd, empty, k_now).error(), expected);
        EXPECT_EQ(market_refusal(market), expected);
    }
    EXPECT_FALSE(market_refusal(MarketType::Spot).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());

    // A served market whose id is genuinely not in the table is a DIFFERENT
    // answer, and the two must not be confused.
    OrderCmd perp{};
    perp.instrument = InstrumentId::make(VenueId::Bitmex, MarketType::Perp, SymbolId{9});
    EXPECT_EQ(builder.create_order(perp, empty, k_now).error(), BuildError::UnknownInstrument);
}

TEST(BitmexRest, WithoutCredentialsNothingIsBuiltAndNothingIsSigned) {
    const auto table = loaded_table();
    net::Signer signer;
    SignedRequestBuilder builder{signer};
    EXPECT_EQ(builder.create_order(limit_buy(table), table, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(builder.positions(k_now).error(), BuildError::NotSigned);
}

// ------------------------------------------------------ private wire ----

TEST(BitmexWire, OneReaderTakesTheBareArrayTheObjectAndTheDataWrapper) {
    // POST answers ONE row, GET and DELETE answer an array, and the socket
    // wraps the same rows in `data`. Reading a row out of the wrong wrapper
    // yields an empty result that looks exactly like an empty account.
    const auto single = fixture("rest_order_new.json");
    const auto one = parse_order(single);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->order_id, "b5c5a2d4-1f04-4a3f-b0f3-9e5d5a4f9a11");
    EXPECT_EQ(one->ord_status, "New");

    static OrderBuffer storage;
    const auto array_body = fixture("rest_order_open.json");
    const auto many = parse_orders(array_body, storage);
    ASSERT_TRUE(many.has_value());
    EXPECT_EQ(many->size(), 3U);

    const auto frame = fixture("ws_order_partial.json");
    const auto wrapped = parse_orders(frame, storage);
    ASSERT_TRUE(wrapped.has_value());
    ASSERT_EQ(wrapped->size(), 1U);
    EXPECT_EQ(wrapped->front().symbol, "XBTUSD");

    const auto info = parse_frame(frame);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->table, TableKind::Order);
    EXPECT_EQ(info->action, FrameAction::Partial);
}

TEST(BitmexWire, AnUpdateCarriesOnlyTheColumnsThatChanged) {
    // BitMEX keys `order` on orderID ALONE, so an update can arrive with no
    // symbol, no side and no size. Reading an absent column as zero would
    // report a live order as one for nothing at a price of nothing.
    const auto frame = fixture("ws_order_update_canceled.json");
    static OrderBuffer storage;
    const auto rows = parse_orders(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto& row = rows->front();
    EXPECT_EQ(row.order_id, "b5c5a2d4-1f04-4a3f-b0f3-9e5d5a4f9a11");
    EXPECT_EQ(row.ord_status, "Canceled");
    EXPECT_TRUE(row.symbol.empty());
    EXPECT_TRUE(row.side.empty());
    EXPECT_TRUE(row.price.empty());
    EXPECT_TRUE(row.order_qty.empty());
    EXPECT_EQ(row.leaves_qty, "0");
}

TEST(BitmexMap, StatusDecidesTheEventAndFillsBelongToTheExecutionTable) {
    EXPECT_EQ(effect_of("New"), OrderEffect::Accepted);
    EXPECT_EQ(effect_of("Canceled"), OrderEffect::Canceled);
    EXPECT_EQ(effect_of("Rejected"), OrderEffect::Rejected);
    EXPECT_EQ(effect_of("Expired"), OrderEffect::Expired);
    // The execution table carries the price, the size and the exec id these do
    // not; emitting anything here would either duplicate it or contradict it.
    EXPECT_EQ(effect_of("PartiallyFilled"), OrderEffect::Silent);
    EXPECT_EQ(effect_of("Filled"), OrderEffect::Silent);
    EXPECT_EQ(effect_of("PendingCancel"), OrderEffect::Silent);
    EXPECT_EQ(effect_of(""), OrderEffect::Silent);
}

TEST(BitmexMap, AnOpenOrderRowConvertsAgainstTheContractsOwnSteps) {
    const auto table = loaded_table();
    const auto body = fixture("rest_order_open.json");
    static OrderBuffer storage;
    const auto rows = parse_orders(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    const auto working = map_open_order((*rows)[0], table);
    ASSERT_TRUE(working.has_value());
    EXPECT_EQ(working->status, OrderStatus::Accepted);
    EXPECT_EQ(working->side, Side::Buy);
    EXPECT_EQ(working->price.raw, 640'005); // 64000.5 over the 0.1 tick
    EXPECT_EQ(working->qty.raw, 1);         // 100 contracts over the 100 lot
    EXPECT_EQ(working->leaves.raw, 1);
    EXPECT_EQ(working->exchange_client_order_id.view(), "01J9ZQ8V3T4W5X6Y7Z8A9B0C1D");

    const auto partial = map_open_order((*rows)[1], table);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(partial->qty.raw, 5);
    EXPECT_EQ(partial->leaves.raw, 3);

    // ETHUSD is quanto: the catalogue refuses it because no single-currency
    // contract size exists, so an order on it has no instrument to attach to.
    EXPECT_FALSE(map_open_order((*rows)[2], table).has_value());
}

TEST(BitmexMap, OnlyExecTypeTradeIsAFillAndTheFeeComesFromTheSettlementValue) {
    const auto table = loaded_table();
    const auto body = fixture("rest_execution_history.json");
    static ExecutionBuffer storage;
    const auto rows = parse_executions(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    const auto maker = map_execution((*rows)[0], table, k_ingest);
    ASSERT_TRUE(maker.has_value());
    EXPECT_EQ(maker->type, OrderEventType::Filled);
    EXPECT_EQ(maker->liquidity, Liquidity::Maker);
    EXPECT_EQ(maker->price.raw, 640'005);
    EXPECT_EQ(maker->qty.raw, 1);
    EXPECT_EQ(maker->leaves_qty.raw, 4);
    EXPECT_EQ(maker->exec_id.view(), "1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d");
    // XBTUSD is INVERSE: 100 contracts of 1 USD at 64000.5 settle in BTC, so
    // 100/64000.5 = 0.001562488 BTC and the 0.00075 rate on it is 1171e-9 BTC.
    // price * quantity would have been 6 400 050 of nothing at all.
    EXPECT_EQ(maker->fee.raw, 1'171);

    // A Funding row carries a commission and a timestamp too; booking it as a
    // trade invents a position that was never traded.
    EXPECT_FALSE(map_execution((*rows)[1], table, k_ingest).has_value());

    const auto taker = map_execution((*rows)[2], table, k_ingest);
    ASSERT_TRUE(taker.has_value());
    EXPECT_EQ(taker->liquidity, Liquidity::Taker);
    EXPECT_EQ(taker->qty.raw, 4);
    EXPECT_EQ(taker->leaves_qty.raw, 0);
    // A rebate is NEGATIVE, and dropping the sign turns a credit into a charge.
    EXPECT_LT(taker->fee.raw, 0);

    const auto report = map_fill((*rows)[0], table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->exec_id, maker->exec_id);
    EXPECT_EQ(report->qty.raw, 1);
    EXPECT_EQ(report->fee.raw, 1'171);
}

TEST(BitmexMap, APositionsSignIsItsSideAndSpotIsNotThePerp) {
    const auto table = loaded_table();
    const auto body = fixture("rest_position.json");
    static PositionBuffer storage;
    const auto rows = parse_positions(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 3U);

    const auto shrt = map_position((*rows)[0], table);
    ASSERT_TRUE(shrt.has_value());
    // currentQty is signed and there is no side field that could disagree.
    EXPECT_EQ(shrt->net_qty.raw, -5);
    EXPECT_EQ(shrt->avg_entry.raw, 635'005);
    EXPECT_EQ(shrt->instrument.market(), MarketType::Perp);

    // Spot and the perpetual are different instruments with different steps;
    // one call answers for both markets, so the caller filters on this.
    const auto spot = map_position((*rows)[1], table);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->instrument.market(), MarketType::Spot);
    EXPECT_EQ(spot->net_qty.raw, 5);
    EXPECT_EQ(spot->avg_entry.raw, 15'025);

    EXPECT_FALSE(map_position((*rows)[2], table).has_value()); // quanto, never loaded
}

TEST(BitmexWs, ThePrivateHandshakeSignsTheSocketsOwnVerbAndPath) {
    std::array<char, 64> message{};
    // The same shape a REST request signs, with no body to follow it.
    EXPECT_EQ(auth_message(1'518'064'236, message), "GET/realtime1518064236");
    std::array<char, 8> small{};
    EXPECT_TRUE(auth_message(1'518'064'236, small).empty());

    Protocol protocol;
    const auto frame = protocol.auth_frame("k-public", 1'518'064'236, "deadbeef");
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text,
              R"({"op":"authKeyExpires","args":["k-public",1518064236,"deadbeef"]})");
    EXPECT_FALSE(protocol.auth_frame("", 1'518'064'236, "deadbeef").has_value());
    EXPECT_FALSE(protocol.auth_frame("k", 0, "deadbeef").has_value());
    EXPECT_FALSE(protocol.auth_frame("k", 1'518'064'236, "").has_value());

    // The private tables are ACCOUNT-scoped: "order", not "order:XBTUSD".
    // Naming a symbol there loses every other contract's orders.
    const std::array<std::string_view, 3> tables{k_table_order, k_table_execution,
                                                 k_table_position};
    const auto subscribe = protocol.private_subscribe_frame(tables);
    ASSERT_TRUE(subscribe.has_value());
    EXPECT_EQ(subscribe->text,
              R"({"op":"subscribe","args":["order","execution","position"]})");
    EXPECT_FALSE(protocol.private_subscribe_frame({}).has_value());

    // A refused handshake is an error frame, not a silent socket — reading it
    // as anything else leaves an authenticated stream indistinguishable from an
    // idle account.
    const auto refusal = fixture("ws_auth_failure.json");
    const auto info = parse_frame(refusal);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->table, TableKind::Error);
    const auto err = parse_stream_error(refusal);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->status, 401);
    EXPECT_EQ(classify(err->status, {}), exec::ErrorClass::AuthFailure);
}

} // namespace
} // namespace raikiri::venues::bitmex
