#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace raikiri::venues::bingx {
namespace {

std::string load_bytes(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The JSON fixtures carry a trailing newline so the files are well-formed text;
// the frames BingX sends do not.
std::string load(std::string_view name) {
    auto text = load_bytes(name);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// Every online row in the contracts fixture, numbered the way a live catalogue
// load numbers them.
exec::InstrumentTable catalogue() {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (const auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

const InstrumentBase& base_for(const exec::InstrumentTable& table, std::string_view symbol) {
    const auto* instrument = table.find(symbol);
    EXPECT_NE(instrument, nullptr);
    return base_of(*instrument);
}

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
};

TEST(BingxVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Bingx, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bingx);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(BingxVenue, OrderWithoutAnImplementationFailsLoudly) {
    // Silence would leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Bingx, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(BingxVenue, PullsThatCannotBeAnsweredReportTerminal) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::Terminal);
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

// ------------------------------------------------------- decompression ----

TEST(BingxInflate, RestoresACompressedFrame) {
    // The whole venue rests on this: the socket delivers gzip, and this is the
    // only place it stops being gzip.
    Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_depth5.frame.gz"));
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());
    EXPECT_EQ(*frame, load("ws_depth5.json"));
}

TEST(BingxInflate, RestoresTheKeepalive) {
    Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_ping.frame.gz"));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, "Ping");
}

TEST(BingxInflate, ReusesOneStreamAcrossFrames) {
    // reset() between frames, or the second one decodes against the first's
    // window and produces plausible garbage.
    Inflater inflater;
    const auto depth = load_bytes("ws_depth5.frame.gz");
    const auto ping = load_bytes("ws_ping.frame.gz");
    for (int i = 0; i < 3; ++i) {
        const auto first = inflater.inflate(depth);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, load("ws_depth5.json"));
        const auto second = inflater.inflate(ping);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*second, "Ping");
    }
}

TEST(BingxInflate, RefusesWhatIsNotGzip) {
    // If BingX ever sent plain text, silently treating it as a frame would be
    // worse than refusing: the parsers would see nothing and the venue would
    // look quiet rather than broken.
    Inflater inflater;
    const auto plain = inflater.inflate(load("ws_book_ticker.json"));
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error(), InflateError::NotGzip);
    EXPECT_EQ(inflater.inflate("").error(), InflateError::Truncated);
}

TEST(BingxInflate, RefusesATruncatedFrame) {
    Inflater inflater;
    const auto whole = load_bytes("ws_depth5.frame.gz");
    const auto header_only = inflater.inflate(std::string_view{whole}.substr(0, 12));
    ASSERT_FALSE(header_only.has_value());
    const auto half = inflater.inflate(std::string_view{whole}.substr(0, whole.size() / 2));
    ASSERT_FALSE(half.has_value());
    EXPECT_EQ(half.error(), InflateError::Corrupt);
}

// ------------------------------------------------------------ ws wire ----

TEST(BingxWire, ClassifiesEveryChannelAndItsSymbol) {
    struct Row {
        std::string_view fixture;
        EventKind kind;
    };

    for (const auto& row : {Row{"ws_book_ticker.json", EventKind::BookTicker},
                            Row{"ws_trade.json", EventKind::Trade},
                            Row{"ws_depth5.json", EventKind::Depth},
                            Row{"ws_mark_price.json", EventKind::MarkPrice}}) {
        const auto frame = load(row.fixture);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << row.fixture;
        EXPECT_EQ(event->kind, row.kind) << row.fixture;
        // Recovered from dataType, including for depth, whose payload never
        // names the instrument.
        EXPECT_EQ(event->symbol, "BTC-USDT") << row.fixture;
    }
}

TEST(BingxWire, ClassifiesTheKeepaliveWhichIsNotJson) {
    const auto event = parse_event("Ping");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Ping);
    EXPECT_TRUE(event->symbol.empty());
}

TEST(BingxWire, SeparatesAnAcknowledgementFromARefusal) {
    // Every frame is a named string: the parsers hand back views INTO it, so a
    // temporary here would dangle the fields under test.
    const auto ack_frame = load("ws_subscribe_ack.json");
    const auto ack = parse_event(ack_frame);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::ControlAck);

    const auto frame = load("ws_error.json");
    const auto refusal = parse_event(frame);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->kind, EventKind::Error);

    const auto control = parse_control(frame);
    ASSERT_TRUE(control.has_value());
    EXPECT_EQ(control->code, k_ws_data_type_unsupported);
    EXPECT_EQ(control->msg, "symbol:NOSUCH-USDT is not supported in websocket");
    EXPECT_EQ(control->id, "e745cd6d-5555-4a70-8f5c-c72cf5b5b6b9");

    // The same code answers an unknown channel, so the message is the only
    // thing that tells the two apart.
    const auto other_frame = load("ws_error_data_type.json");
    const auto other = parse_control(other_frame);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other->code, k_ws_data_type_unsupported);
    EXPECT_EQ(other->msg, "dataType not support");
}

TEST(BingxWire, ParsesBookTicker) {
    const auto frame = load("ws_book_ticker.json");
    const auto wire = parse_book_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->bid_price, "64120.1");
    EXPECT_EQ(wire->bid_qty, "11.6832");
    EXPECT_EQ(wire->ask_price, "64120.2");
    EXPECT_EQ(wire->ask_qty, "136.1929");
    EXPECT_EQ(wire->update_id, 278387392ULL);
    EXPECT_EQ(wire->transaction_ms, 1786394757655ULL);
    EXPECT_EQ(wire->event_ms, 1786394757658ULL);
}

TEST(BingxWire, ParsedViewsPointIntoTheCallerBuffer) {
    // The rebase contract: a view that pointed into simdjson's scratch would
    // dangle the moment the next frame arrived.
    const auto frame = load("ws_book_ticker.json");
    const auto wire = parse_book_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto* begin = frame.data();
    const auto* end = frame.data() + frame.size();
    for (const auto field : {wire->symbol, wire->bid_price, wire->ask_qty}) {
        EXPECT_GE(field.data(), begin);
        EXPECT_LE(field.data() + field.size(), end);
    }
}

TEST(BingxWire, ParsesTrades) {
    TradeBuffer storage;
    const auto frame = load("ws_trade.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);
    EXPECT_EQ((*wire)[0].symbol, "BTC-USDT");
    EXPECT_EQ((*wire)[0].price, "64120.2");
    EXPECT_EQ((*wire)[0].qty, "0.0010");
    EXPECT_EQ((*wire)[0].trade_ms, 1786394757606ULL);
    EXPECT_FALSE((*wire)[0].buyer_is_maker);
}

TEST(BingxWire, ParsesDepthSnapshot) {
    LevelBuffer storage;
    const auto frame = load("ws_depth5.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->ts_ms, 1786394757698ULL); // beside `data`, not inside it
    ASSERT_EQ(wire->bids.size(), 5U);
    ASSERT_EQ(wire->asks.size(), 5U);
    EXPECT_EQ(wire->bids[0].price, "64120.1");
    EXPECT_EQ(wire->bids[4].price, "64119.4"); // descending
    EXPECT_EQ(wire->asks[0].price, "64120.2");
    EXPECT_EQ(wire->asks[4].price, "64121.3"); // ascending
    EXPECT_EQ(wire->asks[0].qty, "136.1929");
}

TEST(BingxWire, RefusesAFrameOfTheWrongShape) {
    // A parser aimed at the wrong channel must fail, not fill a struct with
    // plausible zeros.
    EXPECT_FALSE(parse_book_ticker(load("ws_trade.json")).has_value());
    EXPECT_FALSE(parse_mark_price(load("ws_depth5.json")).has_value());
    EXPECT_FALSE(parse_event("{\"dataType\"").has_value());
    EXPECT_FALSE(parse_event("").has_value());

    LevelBuffer storage;
    EXPECT_FALSE(parse_depth(load("ws_book_ticker.json"), storage).has_value());
}

TEST(BingxWire, ParsesMarkPrice) {
    const auto frame = load("ws_mark_price.json");
    const auto wire = parse_mark_price(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->mark_price, "64124.8");
    EXPECT_EQ(wire->event_ms, 1786394757701ULL);
}

// ---------------------------------------------------------- rest wire ----

TEST(BingxWire, ReadsTheRestEnvelope) {
    const auto ok_body = load("rest_contracts.json");
    const auto ok = parse_rest_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->code, k_ok);

    const auto bad_body = load("rest_error_bad_symbol.json");
    const auto bad_symbol = parse_rest_envelope(bad_body);
    ASSERT_TRUE(bad_symbol.has_value());
    EXPECT_EQ(bad_symbol->code, k_symbol_not_found);
    EXPECT_TRUE(bad_symbol->msg.starts_with("NOSUCH-USDT not exist"));

    // The failure shape that drops `data` entirely and carries `timestamp`.
    const auto missing_body = load("rest_error_not_found.json");
    const auto missing = parse_rest_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_endpoint_not_found);
}

TEST(BingxWire, RefusesAnEnvelopeWithoutACode) {
    // Zero is success, so an unreadable code must never decay into one.
    EXPECT_FALSE(parse_rest_envelope(R"({"msg":"","data":[]})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"0","msg":""})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json").has_value());
}

TEST(BingxWire, ParsesContracts) {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    EXPECT_EQ((*rows)[0].symbol, "BTC-USDT");
    EXPECT_EQ((*rows)[0].status, 1);
    EXPECT_EQ((*rows)[0].price_precision, 1);
    EXPECT_EQ((*rows)[0].quantity_precision, 4);
    EXPECT_EQ((*rows)[0].trade_min_usdt, "2");

    // `size` contradicts the precision on this row and the precision is the
    // one that matches live quantities.
    EXPECT_EQ((*rows)[2].symbol, "SOL-USDT");
    EXPECT_EQ((*rows)[2].size, "\"1\"");
    EXPECT_EQ((*rows)[2].quantity_precision, 2);

    EXPECT_EQ((*rows)[3].status, 25);
    // Scientific notation, straight from the venue: carried as a token because
    // parse_decimal_e9 refuses it.
    EXPECT_EQ((*rows)[3].trade_min_quantity, "7e-05");
}

TEST(BingxWire, ParsesPremiumIndexAndServerTime) {
    const auto premium_body = load("rest_premium_index.json");
    const auto premium = parse_premium_index(premium_body);
    ASSERT_TRUE(premium.has_value());
    EXPECT_EQ(premium->symbol, "BTC-USDT");
    EXPECT_EQ(premium->mark_price, "64076.5");
    EXPECT_EQ(premium->index_price, "64104.7");
    EXPECT_EQ(premium->last_funding_rate, "0.00010000");
    EXPECT_EQ(premium->next_funding_ms, 1786406400000ULL);
    EXPECT_EQ(premium->funding_interval_hours, 8);

    const auto time = parse_server_time(load("rest_server_time.json"));
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, 1786395813183ULL);
    EXPECT_FALSE(parse_server_time(load("rest_error_not_found.json")).has_value());
}

// ---------------------------------------------------------------- map ----

TEST(BingxMap, DerivesStepsAsExactPowersOfTen) {
    EXPECT_EQ(step_from_precision(0)->raw, k_e9);        // 1
    EXPECT_EQ(step_from_precision(1)->raw, 100'000'000); // 0.1
    EXPECT_EQ(step_from_precision(4)->raw, 100'000);     // 0.0001
    EXPECT_EQ(step_from_precision(8)->raw, 10);          // 0.00000001
    EXPECT_EQ(step_from_precision(9)->raw, 1);
    // Past what FixedE9 can hold: refused rather than rounded to zero, which
    // would make every later division by the step a division by zero.
    EXPECT_FALSE(step_from_precision(10).has_value());
    EXPECT_FALSE(step_from_precision(-1).has_value());
}

TEST(BingxMap, BuildsInstrumentsAndRefusesTheOnesItCannotTrade) {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_instrument((*rows)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base.tick_size.raw, 100'000'000); // 10^-1
    EXPECT_EQ(base.lot_size.raw, 100'000);      // 10^-4
    EXPECT_EQ(base.contract_value.raw, k_e9);   // base units, not contracts
    EXPECT_EQ(base.min_notional.raw, 2 * k_e9); // tradeMinUSDT
    EXPECT_TRUE(std::holds_alternative<PerpInstrument>(*btc));
    // Not on the catalogue row; map_funding puts it on the update instead.
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 0U);

    const auto sol = map_instrument((*rows)[2], MarketType::Perp);
    ASSERT_TRUE(sol.has_value());
    EXPECT_EQ(base_of(*sol).tick_size.raw, 1'000'000); // 10^-3
    EXPECT_EQ(base_of(*sol).lot_size.raw, 10'000'000); // 10^-2, not `size` of 1

    // status 25: listed, complete, and not trading. It ships usable precisions,
    // so only the status check keeps it out.
    EXPECT_FALSE(map_instrument((*rows)[3], MarketType::Perp).has_value());
    // The contracts endpoint is the perpetual catalogue and nothing else.
    EXPECT_FALSE(map_instrument((*rows)[0], MarketType::Spot).has_value());
}

TEST(BingxMap, MapsBbo) {
    const auto table = catalogue();
    const auto frame = load("ws_book_ticker.json");
    const auto wire = parse_book_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, UnixNanos{99});
    ASSERT_TRUE(bbo.has_value());
    const auto& base = base_for(table, "BTC-USDT");
    EXPECT_EQ(bbo->instrument.packed, base.id.packed);
    EXPECT_EQ(bbo->bid.raw, 641201); // 64120.1 / 0.1
    EXPECT_EQ(bbo->ask.raw, 641202);
    EXPECT_EQ(bbo->bid_qty.raw, 116832); // 11.6832 / 0.0001
    EXPECT_EQ(bbo->ask_qty.raw, 1361929);
    // T, not E: when the book changed, not when BingX sent it.
    EXPECT_EQ(bbo->ts_event.ns, 1786394757655ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 99U);
}

TEST(BingxMap, BookSnapshotCrossesIntoTheDomain) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = load("ws_depth5.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_book(*wire, table, UnixNanos{99});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->instrument.packed, base_for(table, "BTC-USDT").id.packed);
    EXPECT_EQ(book->bid_count, 5U);
    EXPECT_EQ(book->ask_count, 5U);
    EXPECT_LE(book->bid_count, k_book_levels);
    EXPECT_LE(book->ask_count, k_book_levels);
    EXPECT_EQ(book->venue_seq, 0U); // depth5 carries no sequence number
    EXPECT_EQ(book->ts_event.ns, 1786394757698ULL * 1'000'000ULL);
    EXPECT_EQ(book->ts_init.ns, 99U);

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

TEST(BingxMap, ALevelOffTheStepTruncatesTheBookRatherThanRoundingIt) {
    const auto table = catalogue();
    const std::array<WireLevel, 3> bids{WireLevel{.price = "64120.1", .qty = "1.0"},
                                        WireLevel{.price = "64119.95", .qty = "1.0"},
                                        WireLevel{.price = "64119.8", .qty = "1.0"}};
    WireDepth wire{};
    wire.symbol = "BTC-USDT";
    wire.bids = bids;
    const auto book = map_book(wire, table, UnixNanos{1});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 1U); // the level below the bad one is unreachable
    EXPECT_EQ(book->ask_count, 0U);

    wire.symbol = "NCCOCOFFEE2USD-USDT"; // never loaded, so no step to divide by
    EXPECT_FALSE(map_book(wire, table, UnixNanos{1}).has_value());
}

TEST(BingxMap, DropsAQuoteThatDoesNotDivideByTheStep) {
    const auto table = catalogue();
    WireBookTicker wire{};
    wire.symbol = "BTC-USDT";
    wire.bid_price = "64120.15"; // finer than the 0.1 tick BingX states
    wire.bid_qty = "11.6832";
    wire.ask_price = "64120.2";
    wire.ask_qty = "136.1929";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    wire.bid_price = "64120.1";
    wire.bid_qty = "11.68325"; // finer than the 0.0001 lot
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // An instrument the catalogue never loaded has no step to divide by.
    wire.bid_qty = "11.6832";
    wire.symbol = "NCCOCOFFEE2USD-USDT";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(BingxMap, TakesTheAggressorFromTheMakerFlag) {
    const auto table = catalogue();
    TradeBuffer storage;
    const auto frame = load("ws_trade.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);

    // m == false: the buyer took the offer.
    const auto bought = map_trade((*wire)[0], table, UnixNanos{7});
    ASSERT_TRUE(bought.has_value());
    EXPECT_EQ(bought->aggressor, Side::Buy);
    EXPECT_EQ(bought->price.raw, 641202);
    EXPECT_EQ(bought->qty.raw, 10); // 0.0010 / 0.0001
    EXPECT_EQ(bought->ts_event.ns, 1786394757606ULL * 1'000'000ULL);

    WireTrade maker = (*wire)[0];
    maker.buyer_is_maker = true;
    const auto sold = map_trade(maker, table, UnixNanos{7});
    ASSERT_TRUE(sold.has_value());
    EXPECT_EQ(sold->aggressor, Side::Sell);
}

TEST(BingxMap, MapsFundingFromThePremiumIndexPoll) {
    const auto table = catalogue();
    const auto body = load("rest_premium_index.json");
    const auto wire = parse_premium_index(body);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, UnixNanos{1234});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001
    EXPECT_EQ(funding->mark_price.raw, 640765);
    EXPECT_EQ(funding->next_funding_time.ns, 1786406400000ULL * 1'000'000ULL);
    // Stated per contract by the venue, unlike okx where it is inferred.
    EXPECT_EQ(funding->interval_s, 8U * 3600U);
    // No forecast exists; echoing the current rate would fabricate one.
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->ts_event.ns, 1234U);

    WirePremiumIndex silent = *wire;
    silent.last_funding_rate = {};
    // No rate at all is not the same as a rate of zero.
    EXPECT_FALSE(map_funding(silent, table, UnixNanos{1}).has_value());
}

// ----------------------------------------------------------------- ws ----

TEST(BingxWs, BuildsSubscriptionFrames) {
    Protocol protocol;
    const auto first = protocol.subscribe_frame("BTC-USDT", data_type_suffix(exec::SubKind::Bbo));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text, R"({"id":"1","reqType":"sub","dataType":"BTC-USDT@bookTicker"})");

    // The id is echoed in the answer, so it has to move.
    const auto second =
        protocol.subscribe_frame("ETH-USDT", data_type_suffix(exec::SubKind::Trades));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text, R"({"id":"2","reqType":"sub","dataType":"ETH-USDT@trade"})");

    const auto depth = protocol.subscribe_frame("BTC-USDT", data_type_suffix(exec::SubKind::Depth));
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->text, R"({"id":"3","reqType":"sub","dataType":"BTC-USDT@depth5@100ms"})");

    const auto off = protocol.unsubscribe_frame("BTC-USDT", "bookTicker");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"id":"4","reqType":"unsub","dataType":"BTC-USDT@bookTicker"})");

    EXPECT_EQ(Protocol::pong_frame(), "Pong");
}

TEST(BingxWs, RefusesToBuildAFrameItCannotName) {
    Protocol protocol;
    // Funding has no channel; sending "" would subscribe to nothing and earn
    // an 80015.
    EXPECT_TRUE(data_type_suffix(exec::SubKind::Funding).empty());
    EXPECT_TRUE(data_type_suffix(exec::SubKind::Kline).empty());
    EXPECT_EQ(protocol.subscribe_frame("BTC-USDT", {}).error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}, "trade").error(), WsBuildError::UnsupportedKind);

    const std::string huge(400, 'X');
    EXPECT_EQ(protocol.subscribe_frame(huge, "bookTicker").error(), WsBuildError::BufferTooSmall);
}

TEST(BingxWs, NamesTheOneMarketDataEndpoint) {
    EXPECT_EQ(stream_host(), "open-api-swap.bingx.com");
    EXPECT_EQ(stream_port(), "443");
    EXPECT_EQ(stream_path(), "/swap-market");
    EXPECT_EQ(rest_host(), "open-api.bingx.com");
}

TEST(BingxRest, BuildsUnauthenticatedReads) {
    RequestBuilder builder;
    EXPECT_EQ(builder.contracts().target, "/openApi/swap/v2/quote/contracts");
    EXPECT_EQ(builder.server_time().target, "/openApi/swap/v2/server/time");
    EXPECT_EQ(builder.premium_index("BTC-USDT").target,
              "/openApi/swap/v2/quote/premiumIndex?symbol=BTC-USDT");
    EXPECT_EQ(builder.depth("BTC-USDT", 5).target,
              "/openApi/swap/v2/quote/depth?symbol=BTC-USDT&limit=5");
    EXPECT_EQ(builder.ticker("ETH-USDT").target, "/openApi/swap/v2/quote/ticker?symbol=ETH-USDT");
    EXPECT_EQ(builder.contracts().method, net::HttpMethod::Get);
}

// ------------------------------------------------------------- errors ----

TEST(BingxErrors, ClassifiesWhatTheVenueActuallySaid) {
    EXPECT_EQ(classify(k_bad_api_key), exec::ErrorClass::AuthFailure);
    // A refused subscription never succeeds on a retry.
    EXPECT_EQ(classify(k_ws_data_type_unsupported), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_symbol_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_parameter), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_endpoint_not_found), exec::ErrorClass::Terminal);
    // Unknown codes are Terminal, never Retryable: retrying an unrecognised
    // refusal at full speed is how an IP earns a ban.
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(0), exec::ErrorClass::Terminal);
}

TEST(BingxErrors, ClassifiesTheHttpStatusWhenThereIsNoEnvelope) {
    EXPECT_EQ(classify_http(429), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify_http(503), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_http(500), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_http(0), exec::ErrorClass::Connectivity);
    EXPECT_EQ(classify_http(200), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_http(404), exec::ErrorClass::Terminal);
}

} // namespace
} // namespace raikiri::venues::bingx
