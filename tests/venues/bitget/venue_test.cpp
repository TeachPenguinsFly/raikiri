#include <raikiri/venues/bitget/bitget_venue.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::venues::bitget {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

// The depth the adapter subscribes at, and Bitget's deepest self-contained
// snapshot: books50 and books100 are both refused with 30016 "Param error".
constexpr std::uint32_t k_bitget_depth = 15;

// The four rows of rest_contracts.json, mapped. PEPEUSDT is expected to be
// refused, so this table holds three.
exec::InstrumentTable table_from_catalogue() {
    exec::InstrumentTable table;
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

const InstrumentBase* base_for(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    return instrument == nullptr ? nullptr : &base_of(*instrument);
}

// ------------------------------------------------------------ wire ------

TEST(BitgetWire, EventDispatchResolvesKindAndInstrument) {
    for (const auto& [file, kind] : {std::pair{"ws_ticker.json", EventKind::Ticker},
                                     std::pair{"ws_trade.json", EventKind::Trade},
                                     std::pair{"ws_books15.json", EventKind::Books},
                                     std::pair{"ws_books_update.json", EventKind::Books}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // Only `arg` names the instrument on every channel: the trade and
        // books elements carry none at all.
        EXPECT_EQ(event->inst_id, "BTCUSDT") << file;
    }
}

TEST(BitgetWire, ControlFramesAreNotMistakenForData) {
    // The two answers differ by one field value. Reading a refusal as an ack
    // turns "the venue rejected this channel" into "this channel is quiet",
    // which is indistinguishable from a market with no activity.
    const auto ack = fixture("ws_subscribe_ack.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);

    const auto bad = fixture("ws_error_unknown_instrument.json");
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);
}

TEST(BitgetWire, StreamErrorCodeIsANumberWhileTheRestCodeIsAString) {
    // The two transports disagree about the type of `code`, so they need
    // separate reads. A string read of the websocket frame yields WrongType
    // on every refusal, which presents as a channel that simply went quiet.
    const auto bad_instrument = fixture("ws_error_unknown_instrument.json");
    auto err = parse_stream_error(bad_instrument);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_unknown_subscription);
    EXPECT_FALSE(err->msg.empty());

    const auto refusal = fixture("rest_error_unknown_symbol.json");
    const auto envelope = parse_rest_envelope(refusal);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, "40034");
}

TEST(BitgetWire, KeepaliveIsExactlyFourBytesAndItsAnswerIsNotJson) {
    // Bitget answers the "ping" text with a bare "pong", so the answer lands
    // on the same ignore path as any frame the venue adds later.
    EXPECT_FALSE(parse_event("pong").has_value());
    EXPECT_EQ(Protocol::ping_frame(), "ping");

    // Verified live: one trailing newline is enough to be refused, which is
    // the frame below. It is also the only live payload here carrying a JSON
    // escape, and the parser hands back the ESCAPED text — unescaping would
    // move the view into a buffer that cannot be rebased onto the caller's.
    const auto refused = fixture("ws_error_unrecognized_request.json");
    const auto err = parse_stream_error(refused);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_unrecognized_request);
    EXPECT_EQ(err->msg, R"(Unrecognized request:ping\n)");
}

TEST(BitgetWire, TickerCarriesBookMarkAndFundingOnOneFrame) {
    const auto frame = fixture("ws_ticker.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->inst_id, "BTCUSDT");
    EXPECT_EQ(ticker->bid_price, "63923.8");
    EXPECT_EQ(ticker->ask_price, "63923.9");
    EXPECT_EQ(ticker->bid_qty, "11.3297");
    EXPECT_EQ(ticker->ask_qty, "15.7309");
    // okx and binance both put these on other channels; here they ride along.
    EXPECT_EQ(ticker->mark_price, "63923.8");
    EXPECT_EQ(ticker->index_price, "63936.84");
    EXPECT_EQ(ticker->funding_rate, "0.0001");
    EXPECT_EQ(ticker->next_funding_ms, 1'786'435'200'000ULL);
    // The element's own ts, not the frame's outer one: they differ by a
    // millisecond on this very frame (…946 against …947).
    EXPECT_EQ(ticker->ts_ms, 1'786'430'817'946ULL);

    // Views point into the caller's buffer, not into parser scratch.
    EXPECT_GE(ticker->bid_price.data(), frame.data());
    EXPECT_LE(ticker->bid_price.data() + ticker->bid_price.size(), frame.data() + frame.size());
}

TEST(BitgetWire, TradesCarryTakerSideDirectly) {
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_trade.json");
    const auto trades = parse_trades(frame, *storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 4U); // one frame, four prints
    const auto& trade = trades->front();
    EXPECT_EQ(trade.inst_id, "BTCUSDT"); // supplied by arg; the element has none
    EXPECT_EQ(trade.trade_id, "1470962960790470662");
    EXPECT_EQ(trade.price, "63923.9");
    EXPECT_EQ(trade.qty, "0.0866");
    EXPECT_EQ(trade.ts_ms, 1'786'430'819'699ULL);
    // "buy" in the captured frame, and it already means the AGGRESSOR bought
    // — no inversion, unlike binance's buyer-was-maker flag. Confirmed
    // against the book: these prints are at the ask.
    EXPECT_TRUE(trade.aggressor_bought);
}

TEST(BitgetWire, BooksChannelsShareOneShapeAndChainOnSeq) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto snapshot_frame = fixture("ws_books15.json");
    const auto snapshot = parse_books(snapshot_frame, *storage);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->snapshot);
    EXPECT_EQ(snapshot->bids.size(), k_bitget_depth);
    EXPECT_EQ(snapshot->asks.size(), k_bitget_depth);
    // books15 restates the whole top of book every frame and pins pseq to 0,
    // so there is nothing to chain onto — 0, where okx uses -1.
    EXPECT_EQ(snapshot->prev_seq_id, 0);
    EXPECT_EQ(snapshot->seq_id, 830'988'172'443);

    // The incremental `books` channel ships the identical element, which is
    // why one parser reads both and Depth can be repointed in one line.
    auto delta_storage = std::make_unique<LevelBuffer>();
    const auto delta_frame = fixture("ws_books_update.json");
    const auto delta = parse_books(delta_frame, *delta_storage);
    ASSERT_TRUE(delta.has_value());
    EXPECT_FALSE(delta->snapshot);
    EXPECT_GT(delta->prev_seq_id, 0);
    EXPECT_GT(delta->seq_id, delta->prev_seq_id);
}

TEST(BitgetWire, DeletedLevelsKeepTheirZeroQuantityAndEmptySidesStayEmpty) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update.json");
    const auto books = parse_books(frame, *storage);
    ASSERT_TRUE(books.has_value());
    bool saw_delete = false;
    for (const auto& level : books->bids) {
        EXPECT_FALSE(level.price.empty());
        EXPECT_FALSE(level.qty.empty()); // "0" is a delete, never an absence
        saw_delete = saw_delete || level.qty == "0";
    }
    EXPECT_TRUE(saw_delete);

    // A one-sided delta is a real live frame, not a malformed one.
    auto one_sided_storage = std::make_unique<LevelBuffer>();
    const auto one_sided_frame = fixture("ws_books_update_one_sided.json");
    const auto one_sided = parse_books(one_sided_frame, *one_sided_storage);
    ASSERT_TRUE(one_sided.has_value());
    EXPECT_TRUE(one_sided->bids.empty());
    EXPECT_EQ(one_sided->asks.size(), 1U);
}

TEST(BitgetWire, RestEnvelopeSucceedsOnTheStringNotTheNumber) {
    for (const char* file : {"rest_contracts.json",
                             "rest_ticker.json",
                             "rest_orderbook.json",
                             "rest_current_fund_rate.json",
                             "rest_server_time.json"}) {
        const auto body = fixture(file);
        const auto envelope = parse_rest_envelope(body);
        ASSERT_TRUE(envelope.has_value()) << file;
        EXPECT_EQ(envelope->code, k_ok) << file;
        // Five characters, not one. Any integer conversion collapses this
        // onto the same zero a missing field produces.
        EXPECT_EQ(envelope->code, "00000") << file;
    }

    const auto missing_product = fixture("rest_error_missing_product_type.json");
    const auto envelope = parse_rest_envelope(missing_product);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_parameter_check_failed);
    EXPECT_NE(envelope->code, k_ok);
}

TEST(BitgetWire, UnreadableEnvelopeIsNotReadAsSuccess) {
    EXPECT_FALSE(parse_rest_envelope(R"({"msg":"","data":null})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"code":null,"msg":"","data":null})").has_value());
    // A numerically-zero code is still not "00000".
    const std::string_view zero{R"({"code":"0","msg":"","data":null})"};
    const auto envelope = parse_rest_envelope(zero);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_NE(envelope->code, k_ok);
}

TEST(BitgetWire, ContractsReportWhatTheVenueSaid) {
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    const auto find = [&](std::string_view symbol) -> const WireContract* {
        for (const auto& row : *rows) {
            if (row.symbol == symbol) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto* btc = find("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->base_coin, "BTC");
    EXPECT_EQ(btc->quote_coin, "USDT");
    EXPECT_EQ(btc->symbol_type, "perpetual");
    EXPECT_EQ(btc->symbol_status, "normal");
    EXPECT_EQ(btc->price_place, "1");
    EXPECT_EQ(btc->price_end_step, "1");
    EXPECT_EQ(btc->size_multiplier, "0.0001");
    EXPECT_EQ(btc->fund_interval, "8");

    // The wire layer reports; deciding a row is unusable belongs to the
    // mapper. SHIBUSDT's quantity step is 10000 while its display precision
    // is 0 decimals, and PEPEUSDT quotes ten price decimals.
    const auto* shib = find("SHIBUSDT");
    ASSERT_NE(shib, nullptr);
    EXPECT_EQ(shib->price_place, "9");
    EXPECT_EQ(shib->size_multiplier, "10000");
    EXPECT_EQ(shib->min_trade_num, "10000");
    const auto* pepe = find("PEPEUSDT");
    ASSERT_NE(pepe, nullptr);
    EXPECT_EQ(pepe->price_place, "10");
}

TEST(BitgetWire, ServerTimeAndFundRateParse) {
    // The body is held, not passed as a temporary: every view a parser
    // returns points into the caller's buffer, so outliving it is the
    // contract.
    const auto time_body = fixture("rest_server_time.json");
    const auto time = parse_server_time(time_body);
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, 1'786'430'803'575ULL);

    const auto funding_body = fixture("rest_current_fund_rate.json");
    const auto funding = parse_current_fund_rate(funding_body);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->symbol, "BTCUSDT");
    EXPECT_EQ(funding->funding_rate, "0.0001");
    // This endpoint alone spells the interval `fundingRateInterval` and the
    // settlement `nextUpdate`; the ticker channel calls the same instant
    // `nextFundingTime`.
    EXPECT_EQ(funding->interval_hours, "8");
    EXPECT_EQ(funding->next_update_ms, 1'786'435'200'000ULL);
    EXPECT_EQ(funding->min_rate, "-0.003");
}

TEST(BitgetWire, RestShapesAreNotWebsocketShapes) {
    auto storage = std::make_unique<LevelBuffer>();

    // /mix/market/orderbook is the one endpoint whose `data` is an OBJECT
    // rather than a single-element array, so it needs its own reader and the
    // websocket one must refuse it.
    const auto book_body = fixture("rest_orderbook.json");
    const auto book = parse_rest_orderbook(book_body, *storage);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bids.size(), 5U);
    EXPECT_EQ(book->asks.size(), 5U);
    EXPECT_EQ(book->ts_ms, 1'786'430'802'539ULL);
    auto ws_storage = std::make_unique<LevelBuffer>();
    EXPECT_FALSE(parse_books(book_body, *ws_storage).has_value());

    // The REST ticker looks like the websocket one and is not: it names the
    // instrument `symbol`, ships no `arg`, and omits nextFundingTime
    // entirely. Sharing a parser between them would silently zero the next
    // settlement time.
    const auto ticker_body = fixture("rest_ticker.json");
    EXPECT_FALSE(parse_ticker(ticker_body).has_value());
}

TEST(BitgetWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto levels = std::make_unique<LevelBuffer>();
    auto trades = std::make_unique<TradeBuffer>();
    auto contracts = std::make_unique<ContractBuffer>();
    for (const char* junk : {"",
                             "{",
                             "[]",
                             "null",
                             R"({"arg":)",
                             "\xff\xfe",
                             R"({"arg":{"channel":"ticker"}})",
                             R"({"arg":{"channel":"ticker","instId":"BTCUSDT"},"data":[{}]})"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trades(frame, *trades).has_value()) << junk;
        EXPECT_FALSE(parse_books(frame, *levels).has_value()) << junk;
        EXPECT_FALSE(parse_contracts(frame, *contracts).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
        EXPECT_FALSE(parse_current_fund_rate(frame).has_value()) << junk;
    }
    // An empty data array is how Bitget answers a query it could not satisfy;
    // inventing a zero-valued reading from it publishes a price of zero.
    EXPECT_FALSE(parse_current_fund_rate(R"({"code":"40034","msg":"x","data":[]})").has_value());
}

// ------------------------------------------------------------- map ------

TEST(BitgetMap, TickComposesPricePlaceWithPriceEndStep) {
    // The claim under test: the step is priceEndStep * 10^-pricePlace, not
    // 10^-pricePlace. Every live row carries priceEndStep "1" today, so only
    // a constructed row can distinguish the two — and a row that changes it
    // would otherwise get a tick five times finer than it trades on, which
    // never drops a message to announce itself.
    WireContract wire{};
    wire.symbol = "FAKEUSDT";
    wire.symbol_type = "perpetual";
    wire.symbol_status = "normal";
    wire.size_multiplier = "1";
    wire.fund_interval = "8";
    wire.price_place = "2";
    wire.price_end_step = "5";
    auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).tick_size.raw, 50'000'000); // 0.05, not 0.01

    // pricePlace 0 is live on one contract, so it must not be mistaken for a
    // missing field.
    wire.price_place = "0";
    wire.price_end_step = "1";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).tick_size.raw, k_e9); // 1

    // Nine decimals is the floor the domain's e9 fixed point can hold; a
    // tenth would floor the unit to a zero step, and a zero step divides
    // every later price conversion.
    wire.price_place = "9";
    EXPECT_TRUE(map_instrument(wire, MarketType::Perp).has_value());
    wire.price_place = "10";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    // A zero or absent priceEndStep would make the tick zero too.
    wire.price_place = "2";
    wire.price_end_step = "0";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    wire.price_end_step = "";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(BitgetMap, CatalogueRowsBecomeInstrumentsOrAreRefused) {
    const auto table = table_from_catalogue();
    // Three of four rows: PEPEUSDT quotes ten price decimals and is refused.
    EXPECT_EQ(table.size(), 3U);
    EXPECT_EQ(table.find("PEPEUSDT"), nullptr);

    const auto* btc = base_for(table, "BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->tick_size.raw, 100'000'000); // pricePlace 1 -> 0.1
    EXPECT_EQ(btc->lot_size.raw, 100'000);      // sizeMultiplier 0.0001
    EXPECT_EQ(btc->min_notional.raw, 5 * k_e9); // minTradeUSDT 5
    EXPECT_EQ(btc->contract_value.raw, k_e9);   // base units, not contracts
    EXPECT_EQ(btc->exchange_symbol.view(), "BTCUSDT");

    const auto* eth = base_for(table, "ETHUSDT");
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(eth->tick_size.raw, 10'000'000); // pricePlace 2 -> 0.01
    EXPECT_EQ(eth->lot_size.raw, 10'000'000);

    // The row that separates sizeMultiplier from volumePlace: SHIBUSDT
    // displays 0 decimals and steps by 10000.
    const auto* shib = base_for(table, "SHIBUSDT");
    ASSERT_NE(shib, nullptr);
    EXPECT_EQ(shib->tick_size.raw, 1); // pricePlace 9 -> 0.000000001
    EXPECT_EQ(shib->lot_size.raw, 10'000 * k_e9);

    // fundInterval is stated by the catalogue, so nothing has to infer it
    // from the gap between two settlement stamps.
    const auto* perp = std::get_if<PerpInstrument>(table.find("BTCUSDT"));
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->funding_interval_s, 28'800U);
}

TEST(BitgetMap, UntradableAndNonPerpetualRowsAreRefused) {
    WireContract wire{};
    wire.symbol = "BTCUSDT";
    wire.symbol_type = "perpetual";
    wire.symbol_status = "normal";
    wire.price_place = "1";
    wire.price_end_step = "1";
    wire.size_multiplier = "0.0001";
    wire.fund_interval = "8";
    ASSERT_TRUE(map_instrument(wire, MarketType::Perp).has_value());

    // A dated contract shares this endpoint on the coin-margined products.
    wire.symbol_type = "delivery";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
    wire.symbol_type = "perpetual";

    // Every non-normal status still ships complete precision fields, so a row
    // that parses cleanly is not a row we may trade.
    for (const char* status : {"maintain", "limit_open", "restrictedAPI", "off", "listed"}) {
        wire.symbol_status = status;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << status;
    }
    wire.symbol_status = "normal";

    // Spot lives behind a different endpoint with a different row shape.
    EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value());
    EXPECT_FALSE(map_instrument(wire, MarketType::Future).has_value());
}

TEST(BitgetMap, TickerBecomesTopOfBook) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    wire.ts_ms = 1'786'430'817'946ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 639'238); // 63923.8 / 0.1
    EXPECT_EQ(bbo->ask.raw, 639'239);
    EXPECT_EQ(bbo->bid_qty.raw, 113'297); // 11.3297 / 0.0001
    EXPECT_EQ(bbo->ask_qty.raw, 157'309);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'430'817'946ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(BitgetMap, BookSnapshotCrossesIntoTheDomain) {
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books15.json");
    const auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_book(*wire, table, k_ingest);
    ASSERT_TRUE(book.has_value());
    // Every level the subscription asked for reaches the domain snapshot; a
    // book that stops at five is the old tier still being served.
    EXPECT_EQ(book->bid_count, k_bitget_depth);
    EXPECT_EQ(book->ask_count, k_bitget_depth);
    static_assert(k_bitget_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 830'988'172'443ULL);
    EXPECT_EQ(book->ts_event.ns, 1'786'983'217'049ULL * 1'000'000ULL);
    EXPECT_EQ(book->ts_init.ns, k_ingest.ns);

    // Bids descend, asks ascend, and the two never cross — a book that fails
    // this is not a book, and publishing it would misprice every strategy
    // reading it.
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw);
    for (std::uint32_t i = 1; i < book->bid_count; ++i) {
        EXPECT_LT(book->bids[i].price.raw, book->bids[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 1; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].price.raw, book->asks[i - 1].price.raw) << i;
    }
    for (std::uint32_t i = 0; i < book->bid_count; ++i) {
        EXPECT_GT(book->bids[i].qty.raw, 0) << i;
    }
    for (std::uint32_t i = 0; i < book->ask_count; ++i) {
        EXPECT_GT(book->asks[i].qty.raw, 0) << i;
    }
}

TEST(BitgetMap, AnIncrementalFrameNeverBecomesABook) {
    // The delta carries deletes as a zero quantity and only the levels that
    // moved; published as a snapshot it would hand shm a book with a hole in
    // it and a top of book 20 away from the real one.
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books_update.json");
    const auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_FALSE(wire->snapshot);
    EXPECT_FALSE(map_book(*wire, table, k_ingest).has_value());
}

TEST(BitgetMap, AnUnknownInstrumentYieldsNoBook) {
    const auto table = table_from_catalogue();
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_books15.json");
    auto wire = parse_books(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    wire->inst_id = "NOSUCHUSDT";
    EXPECT_FALSE(map_book(*wire, table, k_ingest).has_value());
}

TEST(BitgetMap, ValuesOffTheStepAreDroppedNotRounded) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.85"; // 0.05 past a 0.1 tick
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());

    // A quantity off the step is the same schema event as a price off the
    // tick. SHIBUSDT steps by 10000, so 5000 is not a size it can report.
    WireTrade trade{};
    trade.inst_id = "SHIBUSDT";
    trade.price = "0.000004496";
    trade.qty = "5000";
    EXPECT_FALSE(map_trade(trade, table, k_ingest).has_value());
    trade.qty = "2220000"; // a real captured print
    const auto mapped = map_trade(trade, table, k_ingest);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped->qty.raw, 222);
}

TEST(BitgetMap, EmptySideAndUnknownInstrumentAreDropped) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    // A contract with no bid is a real state, and an empty side must not turn
    // into a zero price at the top of book.
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());

    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.inst_id = "PEPEUSDT"; // refused by the catalogue, so never in the table
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(BitgetMap, TakerSideIsCarriedThroughUnflipped) {
    // Bitget states the aggressor's own direction, so this mapper must NOT
    // invert. Copying binance's flip here would reverse every signed print.
    const auto table = table_from_catalogue();
    WireTrade wire{};
    wire.inst_id = "BTCUSDT";
    wire.price = "63923.9";
    wire.qty = "0.0866";
    wire.ts_ms = 1'786'430'819'699ULL;

    wire.aggressor_bought = true;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 639'239);
    EXPECT_EQ(trade->qty.raw, 866);

    wire.aggressor_bought = false;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(BitgetMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_from_catalogue();
    WireTrade wire{};
    wire.inst_id = "BTCUSDT";
    wire.price = "63923.9";
    wire.qty = "0.0866";
    wire.ts_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // Not 1970: a zero timestamp reaching a latency histogram poisons it.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(BitgetMap, FundingRidesTheTickerFrame) {
    const auto table = table_from_catalogue();
    WireTicker wire{};
    wire.inst_id = "BTCUSDT";
    wire.bid_price = "63923.8";
    wire.bid_qty = "11.3297";
    wire.ask_price = "63923.9";
    wire.ask_qty = "15.7309";
    wire.mark_price = "63923.8";
    wire.funding_rate = "0.0001";
    wire.next_funding_ms = 1'786'435'200'000ULL;
    wire.ts_ms = 1'786'430'817'946ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001 * 1e9
    // Mark is real here, unlike okx's, because the same frame carries it.
    EXPECT_EQ(funding->mark_price.raw, 639'238);
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'435'200'000ULL * 1'000'000ULL);
    // Read from the catalogue rather than inferred from two stamps.
    EXPECT_EQ(funding->interval_s, 28'800U);
    // No next-interval forecast exists on this channel, so it stays undefined
    // rather than being published as a predicted rate of zero.
    EXPECT_TRUE(funding->predicted_rate.is_undef());

    // A contract quoting no rate is not a contract funding at zero.
    wire.funding_rate = "";
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

// ----------------------------------------------------------- errors -----

TEST(BitgetErrors, ClockSkewIsRetryableAndUnknownCodesAreTerminal) {
    // The same payload signed against a corrected offset succeeds, so a
    // resync-and-retry is right. AuthFailure would halt a venue whose keys
    // are fine.
    EXPECT_EQ(classify(k_timestamp_expired), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_system_error), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    for (const auto& code : {k_access_key_empty,
                             k_secret_key_empty,
                             k_signature_empty,
                             k_invalid_access_key,
                             k_signature_error}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::AuthFailure) << code;
    }
    // An unknown refusal repeated at full speed is how an account earns an IP
    // ban, so the default is Terminal rather than Retryable.
    for (const auto& code : {k_parameter_not_exist,
                             k_url_not_found,
                             k_parameter_check_failed,
                             std::string_view{"99999"},
                             std::string_view{"not-a-code"},
                             std::string_view{}}) {
        EXPECT_EQ(classify(code), exec::ErrorClass::Terminal) << code;
    }
}

TEST(BitgetErrors, CodeNumberIsForReportingOnly) {
    EXPECT_EQ(code_number("40034"), 40034);
    EXPECT_EQ(code_number("400172"), 400172);
    // "00000" and an unreadable code collide at zero, which is exactly why
    // classification compares text and never this value.
    EXPECT_EQ(code_number(k_ok), 0);
    EXPECT_EQ(code_number("not-a-code"), 0);
}

// --------------------------------------------------------- protocol -----

TEST(BitgetWs, SubscribeFrameCarriesInstType) {
    Protocol protocol;
    const std::array<Subscription, 1> one{
        Subscription{.inst_type = "USDT-FUTURES", .channel = "ticker", .inst_id = "BTCUSDT"}};
    const auto frame = protocol.subscribe_frame(one);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 1U);
    // Byte for byte the frame the live socket acknowledged.
    EXPECT_EQ(
        frame->text,
        R"({"op":"subscribe","args":[{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"}]})");

    const std::array<Subscription, 2> two{
        Subscription{.inst_type = "USDT-FUTURES", .channel = "ticker", .inst_id = "BTCUSDT"},
        Subscription{.inst_type = "USDT-FUTURES", .channel = "trade", .inst_id = "ETHUSDT"}};
    const auto batched = protocol.subscribe_frame(two);
    ASSERT_TRUE(batched.has_value());
    EXPECT_EQ(batched->consumed, 2U); // batching, against a 10 msg/s budget
}

TEST(BitgetWs, IncompleteSubscriptionsAreRefusedRatherThanSent) {
    Protocol protocol;
    // instType is required and not inferable from the symbol: BTCUSDT names a
    // contract under USDT-FUTURES and another under the demo SUSDT-FUTURES.
    const std::array<Subscription, 1> no_type{
        Subscription{.inst_type = {}, .channel = "ticker", .inst_id = "BTCUSDT"}};
    EXPECT_FALSE(protocol.subscribe_frame(no_type).has_value());
    // An empty channel is channel_for() declining the kind; sending it would
    // earn a 30016 Param error.
    const std::array<Subscription, 1> no_channel{
        Subscription{.inst_type = "USDT-FUTURES", .channel = {}, .inst_id = "BTCUSDT"}};
    EXPECT_FALSE(protocol.subscribe_frame(no_channel).has_value());
    EXPECT_FALSE(protocol.subscribe_frame({}).has_value());
}

TEST(BitgetWs, OneChannelServesThreeSubscriptionKinds) {
    EXPECT_EQ(channel_for(exec::SubKind::Bbo), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::MarkPrice), "ticker");
    EXPECT_EQ(channel_for(exec::SubKind::Trades), "trade");
    EXPECT_EQ(channel_for(exec::SubKind::Depth), "books15");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());

    EXPECT_EQ(inst_type(false), "USDT-FUTURES");
    EXPECT_EQ(inst_type(true), "SUSDT-FUTURES");
    EXPECT_EQ(stream_path(), "/v2/ws/public");
}

TEST(BitgetRest, EveryMixTargetCarriesProductType) {
    // Omitting productType does not default: it answers 400172 "Parameter
    // verification failed", which reads like a malformed symbol.
    RequestBuilder builder;
    EXPECT_EQ(builder.contracts(product_type(false)).target,
              "/api/v2/mix/market/contracts?productType=USDT-FUTURES");
    EXPECT_EQ(builder.ticker("BTCUSDT", product_type(false)).target,
              "/api/v2/mix/market/ticker?symbol=BTCUSDT&productType=USDT-FUTURES");
    EXPECT_EQ(builder.current_fund_rate("BTCUSDT", product_type(false)).target,
              "/api/v2/mix/market/current-fund-rate?symbol=BTCUSDT&productType=USDT-FUTURES");
    EXPECT_EQ(builder.orderbook("BTCUSDT", product_type(false), 5).target,
              "/api/v2/mix/market/orderbook?symbol=BTCUSDT&productType=USDT-FUTURES&limit=5");
    // The clock endpoint is the only one that takes no productType.
    EXPECT_EQ(builder.server_time().target, "/api/v2/public/time");
    EXPECT_EQ(product_type(true), "SUSDT-FUTURES");
}

// ------------------------------------------------------------ venue -----

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(BitgetVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Bitget, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bitget);
    EXPECT_NE(adapter->data, nullptr);
}

TEST(BitgetVenue, OrderWithoutAnImplementationFailsLoudly) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bitget, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    // CancelRejected, not Rejected: the order a cancel targets is still alive.
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);

    // Every pull refuses terminally rather than answering with an empty set,
    // which reconciliation would read as "nothing is open".
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
    EXPECT_EQ(adapter->execution->open_order_reports(MarketType::Perp).error().cls,
              exec::ErrorClass::Terminal);
}

TEST(BitgetVenue, SpotCatalogueRefusesRatherThanReturningNothing) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bitget, .enable_spot = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    // An empty catalogue would look like a venue that lists no spot symbols.
    const auto loaded = adapter->instruments->fetch_all(MarketType::Spot);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().cls, exec::ErrorClass::Terminal);
}

} // namespace
} // namespace raikiri::venues::bitget
