#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/bingx_venue.hpp>
#include <raikiri/venues/bingx/client_id.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/venues/bingx/map.hpp>
#include <raikiri/venues/bingx/rest.hpp>
#include <raikiri/venues/bingx/ws.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::bingx {
namespace {

// The level count in the depth channel's name, and BingX's deepest tier:
// depth200 draws 80015 "dataType not support".
constexpr std::uint32_t k_bingx_depth = 100;

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
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bingx);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(BingxVenue, AnOrderWithNoCredentialsIsRefusedLoudly) {
    // Silence would leave the order open in the store forever, and an adapter
    // with no key must say so at the first order rather than at the first fill
    // that never arrives.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Bingx, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[0].leaves_qty.raw, 0);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    // CancelRejected, not Rejected: the order this targets is still alive, so
    // its leaves are restated rather than zeroed.
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);

    cmd.kind = CmdKind::Amend;
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
}

TEST(BingxVenue, RefusesAnOptionOrderWithoutBlamingTheCatalogue) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Bingx, MarketType::Option, SymbolId{0});
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    const auto orders = adapter->execution->open_order_reports(MarketType::Option);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().message.view(), "bingx options order entry");
    const auto futures = adapter->execution->open_order_reports(MarketType::Future);
    ASSERT_FALSE(futures.has_value());
    EXPECT_EQ(futures.error().message.view(), "bingx dated futures order entry");
}

TEST(BingxVenue, PullsSayWhyTheyCannotAnswer) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // No credentials is an AuthFailure, not a Terminal one: the gateway halts
    // and says the key is missing rather than retrying forever.
    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::AuthFailure);
    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);

    // A spot balance is not a position, and neither market can answer the fill
    // pull. Each refusal names its own reason so a reconciler cannot read it as
    // "the account is flat".
    const auto spot_positions = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(spot_positions.has_value());
    EXPECT_EQ(spot_positions.error().message.view(), "bingx spot position pull");
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Spot, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->connected());
}

TEST(BingxVenue, EachSocketRefusesTheOtherMarketsInstruments) {
    // Both public endpoints are the swap market's and BingX spells a spot pair
    // the same way, so a spot id subscribed here would be fed perpetual prints
    // under a spot instrument — worse than no data at all.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto spot = adapter->data->subscribe(
        exec::SubKind::Bbo, InstrumentId::make(VenueId::Bingx, MarketType::Spot, SymbolId{0}));
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().cls, exec::ErrorClass::Terminal);

    // And the mirror, which is the half that matters: a spot socket must
    // refuse a perpetual id just as hard. BingX spells the two markets with
    // the same ticker, so a wrong-market id is not rejected by the venue —
    // it comes back subscribed, to the other tree's prices.
    auto spot_adapter =
        exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(spot_adapter.has_value());
    const auto perp = spot_adapter->data->subscribe(
        exec::SubKind::Bbo, InstrumentId::make(VenueId::Bingx, MarketType::Perp, SymbolId{0}));
    ASSERT_FALSE(perp.has_value());
    EXPECT_EQ(perp.error().cls, exec::ErrorClass::Terminal);
}

// ------------------------------------------------------- decompression ----

TEST(BingxInflate, RestoresACompressedFrame) {
    // The whole venue rests on this: the socket delivers gzip, and this is the
    // only place it stops being gzip.
    net::Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_depth100.frame.gz"));
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());
    EXPECT_EQ(*frame, load("ws_depth100.json"));
}

TEST(BingxInflate, RestoresTheKeepalive) {
    net::Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_ping.frame.gz"));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, "Ping");
}

TEST(BingxInflate, ReusesOneStreamAcrossFrames) {
    // reset() between frames, or the second one decodes against the first's
    // window and produces plausible garbage.
    net::Inflater inflater;
    const auto depth = load_bytes("ws_depth100.frame.gz");
    const auto ping = load_bytes("ws_ping.frame.gz");
    for (int i = 0; i < 3; ++i) {
        const auto first = inflater.inflate(depth);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, load("ws_depth100.json"));
        const auto second = inflater.inflate(ping);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*second, "Ping");
    }
}

TEST(BingxInflate, RefusesWhatIsNotGzip) {
    // If BingX ever sent plain text, silently treating it as a frame would be
    // worse than refusing: the parsers would see nothing and the venue would
    // look quiet rather than broken.
    net::Inflater inflater;
    const auto plain = inflater.inflate(load("ws_book_ticker.json"));
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error(), net::InflateError::NotGzip);
    EXPECT_EQ(inflater.inflate("").error(), net::InflateError::Truncated);
}

TEST(BingxInflate, RefusesATruncatedFrame) {
    net::Inflater inflater;
    const auto whole = load_bytes("ws_depth100.frame.gz");
    const auto header_only = inflater.inflate(std::string_view{whole}.substr(0, 12));
    ASSERT_FALSE(header_only.has_value());
    const auto half = inflater.inflate(std::string_view{whole}.substr(0, whole.size() / 2));
    ASSERT_FALSE(half.has_value());
    EXPECT_EQ(half.error(), net::InflateError::Corrupt);
}

// ------------------------------------------------------------ ws wire ----

TEST(BingxWire, ClassifiesEveryChannelAndItsSymbol) {
    struct Row {
        std::string_view fixture;
        EventKind kind;
    };

    for (const auto& row : {Row{"ws_book_ticker.json", EventKind::BookTicker},
                            Row{"ws_trade.json", EventKind::Trade},
                            Row{"ws_depth100.json", EventKind::Depth},
                            Row{"ws_mark_price.json", EventKind::MarkPrice},
                            // "ticker" and "bookTicker" are two channels one
                            // suffix compare apart.
                            Row{"ws_ticker.json", EventKind::Ticker}}) {
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
    const auto frame = load("ws_depth100.json");
    const auto wire = parse_depth(frame, storage, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->ts_ms, 1786983056064ULL); // beside `data`, not inside it
    ASSERT_EQ(wire->bids.size(), k_bingx_depth);
    ASSERT_EQ(wire->asks.size(), k_bingx_depth);
    EXPECT_EQ(wire->bids[0].price, "64071.2");
    EXPECT_EQ(wire->bids[k_bingx_depth - 1].price, "64005.6"); // descending
    EXPECT_EQ(wire->asks[0].price, "64071.3");
    EXPECT_EQ(wire->asks[k_bingx_depth - 1].price, "64162.1"); // ascending
    EXPECT_EQ(wire->asks[0].qty, "78.6122");
}

TEST(BingxWire, RefusesAFrameOfTheWrongShape) {
    // A parser aimed at the wrong channel must fail, not fill a struct with
    // plausible zeros.
    EXPECT_FALSE(parse_book_ticker(load("ws_trade.json")).has_value());
    EXPECT_FALSE(parse_mark_price(load("ws_depth100.json")).has_value());
    EXPECT_FALSE(parse_event("{\"dataType\"").has_value());
    EXPECT_FALSE(parse_event("").has_value());

    LevelBuffer storage;
    EXPECT_FALSE(parse_depth(load("ws_book_ticker.json"), storage, MarketType::Perp).has_value());
}

TEST(BingxWire, ParsesMarkPrice) {
    const auto frame = load("ws_mark_price.json");
    const auto wire = parse_mark_price(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->mark_price, "64124.8");
    EXPECT_EQ(wire->event_ms, 1786394757701ULL);
}

TEST(BingxWire, TickerCarriesBothTurnoverColumns) {
    const auto frame = load("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    // `v` is BASE and `q` is QUOTE: their quotient, 80703.8, sits between this
    // frame's low (78949.9) and high (82264.7). Quoted here and bare on spot,
    // which is why both are read as scalars.
    EXPECT_EQ(wire->volume, "21355.7400");
    EXPECT_EQ(wire->quote_volume, "1723468750.41");
    EXPECT_EQ(wire->event_ms, 1788532398984ULL);

    // BingX pushes a whole snapshot every second, but a frame that stated
    // neither column would still have to parse: nothing else on it depends on
    // the turnover, and empty is what the mapper reads as "said nothing".
    const auto empty = parse_ticker(R"({"code":0,"dataType":"BTC-USDT@ticker",)"
                                    R"("data":{"e":"24hTicker","E":1788532398984,)"
                                    R"("s":"BTC-USDT","c":"79343.2"}})");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->volume.empty());
    EXPECT_TRUE(empty->quote_volume.empty());
}

TEST(BingxMap, TurnoverResolvesAgainstTheSocketsOwnMarket) {
    const auto table = catalogue();
    const auto frame = load("ws_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());

    const auto stats = map_ticker_stats(*wire, table, MarketType::Perp, UnixNanos{99});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 21'355'740'000'000LL);         // 21355.7400 BTC
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'723'468'750'410'000'000LL); // 1723468750.41 USDT
    EXPECT_EQ(stats->ts_event.ns, 1788532398984ULL * 1'000'000ULL);
    EXPECT_EQ(stats->ts_init.ns, 99U);

    // Same spelling on both trees: a swap frame resolved against the spot
    // market must yield nothing rather than the wrong instrument's turnover.
    EXPECT_FALSE(map_ticker_stats(*wire, table, MarketType::Spot, UnixNanos{99}).has_value());

    WireTicker silent = *wire;
    silent.volume = {};
    silent.quote_volume = {};
    EXPECT_FALSE(map_ticker_stats(silent, table, MarketType::Perp, UnixNanos{1}).has_value());
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

TEST(BingxMap, TheCanonicalPairIsSplitOutOfTheBingxSymbol) {
    // BingX carries the pair in the symbol and in no field beside it, so this
    // split is the only thing that lets a cross-venue view put BingX's
    // `BTC-USDT` beside Bybit's `BTCUSDT`.
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());
    const auto perp = map_instrument((*rows)[0], MarketType::Perp);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(base_of(*perp).exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base_of(*perp).local_symbol.view(), "BTC/USDT");

    static thread_local SpotSymbolBuffer spot_storage;
    const auto spot_body = load("rest_spot_symbols.json");
    const auto spot_rows = parse_spot_symbols(spot_body, spot_storage);
    ASSERT_TRUE(spot_rows.has_value());
    const auto spot = map_spot_instrument((*spot_rows)[0]);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(base_of(*spot).exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base_of(*spot).local_symbol.view(), "BTC/USDT");
}

TEST(BingxMap, ARowWhosePairCannotBeReadIsDropped) {
    WireContract wire{};
    wire.symbol = "BTC-USDT";
    wire.status = 1;
    wire.price_precision = 1;
    wire.quantity_precision = 4;
    ASSERT_TRUE(map_instrument(wire, MarketType::Perp).has_value());

    // A symbol whose pair cannot be read is dropped: no separator, an empty
    // half, a trailing kind that makes three parts. A row nothing can group
    // would show in a cross-venue view as a market of its own.
    for (const char* symbol : {"BTCUSDT", "BTC-USDT-SWAP", "-USDT", "BTC-", ""}) {
        wire.symbol = symbol;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << symbol;
    }

    // `NCCOCOFFEE2USD-USDT` is live on this catalogue and its base is fourteen
    // characters. It must LOAD: dropping a listing because its ticker is long
    // is a market silently missing from every view, which is what widening
    // AssetCode past twelve was for.
    wire.symbol = "NCCOCOFFEE2USD-USDT";
    const auto commodity = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(commodity.has_value());
    EXPECT_EQ(base_of(*commodity).local_symbol.view(), "NCCOCOFFEE2USD/USDT");

    WireSpotSymbol row{};
    row.symbol = "BTC-USDT";
    row.tick_size = "0.01";
    row.step_size = "0.000001";
    row.status = 1;
    row.api_buy = true;
    row.api_sell = true;
    ASSERT_TRUE(map_spot_instrument(row).has_value());
    row.symbol = "BTCUSDT";
    EXPECT_FALSE(map_spot_instrument(row).has_value());
}

TEST(BingxMap, MapsBbo) {
    const auto table = catalogue();
    const auto frame = load("ws_book_ticker.json");
    const auto wire = parse_book_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, MarketType::Perp, UnixNanos{99});
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
    const auto frame = load("ws_depth100.json");
    const auto wire = parse_depth(frame, storage, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_book(*wire, table, MarketType::Perp, UnixNanos{99});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->instrument.packed, base_for(table, "BTC-USDT").id.packed);
    // Every level the subscription asked for reaches the domain snapshot; a
    // book that stops at five is the old tier still being served.
    EXPECT_EQ(book->bid_count, k_bingx_depth);
    EXPECT_EQ(book->ask_count, k_bingx_depth);
    static_assert(k_bingx_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 0U); // the depth channel carries no sequence number
    EXPECT_EQ(book->ts_event.ns, 1786983056064ULL * 1'000'000ULL);
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
    const auto book = map_book(wire, table, MarketType::Perp, UnixNanos{1});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->bid_count, 1U); // the level below the bad one is unreachable
    EXPECT_EQ(book->ask_count, 0U);

    wire.symbol = "NCCOCOFFEE2USD-USDT"; // never loaded, so no step to divide by
    EXPECT_FALSE(map_book(wire, table, MarketType::Perp, UnixNanos{1}).has_value());
}

TEST(BingxMap, DropsAQuoteThatDoesNotDivideByTheStep) {
    const auto table = catalogue();
    WireBookTicker wire{};
    wire.symbol = "BTC-USDT";
    wire.bid_price = "64120.15"; // finer than the 0.1 tick BingX states
    wire.bid_qty = "11.6832";
    wire.ask_price = "64120.2";
    wire.ask_qty = "136.1929";
    EXPECT_FALSE(map_bbo(wire, table, MarketType::Perp, UnixNanos{1}).has_value());

    wire.bid_price = "64120.1";
    wire.bid_qty = "11.68325"; // finer than the 0.0001 lot
    EXPECT_FALSE(map_bbo(wire, table, MarketType::Perp, UnixNanos{1}).has_value());

    // An instrument the catalogue never loaded has no step to divide by.
    wire.bid_qty = "11.6832";
    wire.symbol = "NCCOCOFFEE2USD-USDT";
    EXPECT_FALSE(map_bbo(wire, table, MarketType::Perp, UnixNanos{1}).has_value());
}

TEST(BingxMap, TakesTheAggressorFromTheMakerFlag) {
    const auto table = catalogue();
    TradeBuffer storage;
    const auto frame = load("ws_trade.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 1U);

    // m == false: the buyer took the offer.
    const auto bought = map_trade((*wire)[0], table, MarketType::Perp, UnixNanos{7});
    ASSERT_TRUE(bought.has_value());
    EXPECT_EQ(bought->aggressor, Side::Buy);
    EXPECT_EQ(bought->price.raw, 641202);
    EXPECT_EQ(bought->qty.raw, 10); // 0.0010 / 0.0001
    EXPECT_EQ(bought->ts_event.ns, 1786394757606ULL * 1'000'000ULL);

    WireTrade maker = (*wire)[0];
    maker.buyer_is_maker = true;
    const auto sold = map_trade(maker, table, MarketType::Perp, UnixNanos{7});
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
    const auto first = protocol.subscribe_frame(
        "BTC-USDT", data_type_suffix(exec::SubKind::Bbo, MarketType::Perp));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text, R"({"id":"1","reqType":"sub","dataType":"BTC-USDT@bookTicker"})");

    // The id is echoed in the answer, so it has to move.
    const auto second = protocol.subscribe_frame(
        "ETH-USDT", data_type_suffix(exec::SubKind::Trades, MarketType::Perp));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text, R"({"id":"2","reqType":"sub","dataType":"ETH-USDT@trade"})");

    const auto depth = protocol.subscribe_frame(
        "BTC-USDT", data_type_suffix(exec::SubKind::Depth, MarketType::Perp));
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->text, R"({"id":"3","reqType":"sub","dataType":"BTC-USDT@depth100@100ms"})");

    const auto off = protocol.unsubscribe_frame("BTC-USDT", "bookTicker");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"id":"4","reqType":"unsub","dataType":"BTC-USDT@bookTicker"})");

    EXPECT_EQ(Protocol::pong_frame(), "Pong");
}

TEST(BingxWs, RefusesToBuildAFrameItCannotName) {
    Protocol protocol;
    // Funding has no channel; sending "" would subscribe to nothing and earn
    // an 80015.
    EXPECT_TRUE(data_type_suffix(exec::SubKind::Funding, MarketType::Perp).empty());
    EXPECT_TRUE(data_type_suffix(exec::SubKind::Kline, MarketType::Perp).empty());
    EXPECT_EQ(protocol.subscribe_frame("BTC-USDT", {}).error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}, "trade").error(), WsBuildError::UnsupportedKind);

    const std::string huge(400, 'X');
    EXPECT_EQ(protocol.subscribe_frame(huge, "bookTicker").error(), WsBuildError::BufferTooSmall);
}

TEST(BingxWs, NamesTheOneMarketDataEndpoint) {
    EXPECT_EQ(stream_host(MarketType::Perp), "open-api-swap.bingx.com");
    EXPECT_EQ(stream_port(), "443");
    EXPECT_EQ(stream_path(MarketType::Perp), "/swap-market");
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

// ---------------------------------------------------------- client id ----

// A ULID the OMS could have minted, and the lowercase form BingX echoes back.
constexpr std::string_view k_ulid = "01JD7Q0M9WKE6Q3N2F4H8ZT5XR";
constexpr std::string_view k_ulid_echo = "01jd7q0m9wke6q3n2f4h8zt5xr";
// A second id, for the spot fixtures — where BingX echoes it UNCHANGED.
constexpr std::string_view k_ulid_spot = "01JD7Q0M9WKE6Q3N2F4H8ZT5XT";

ExchangeClientOrderId ulid(std::string_view text = k_ulid) {
    const auto id = ExchangeClientOrderId::from(text);
    EXPECT_TRUE(id.has_value());
    return id.value_or(ExchangeClientOrderId{});
}

TEST(BingxClientId, CarriesAUlidWholeAndUndoesTheVenuesLowercasing) {
    // BingX's field holds 40 characters, so nothing is re-encoded here — but
    // it folds the value to lower case, and an echo compared without folding
    // back makes every one of our own orders look like a stranger's.
    EXPECT_EQ(client_id_text(ulid()), k_ulid);
    const auto decoded = client_id_from_text(k_ulid_echo);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->view(), k_ulid);
    EXPECT_EQ(client_id_from_text(k_ulid)->view(), k_ulid);
}

TEST(BingxClientId, RefusesAnythingThatIsNotOneOfOurs) {
    // A web order's id must leave the field unset rather than decode into a
    // lookalike of an id we minted.
    EXPECT_FALSE(client_id_from_text("myOrder123").has_value());
    EXPECT_FALSE(client_id_from_text("").has_value());
    // I, L, O and U are not in Crockford base32.
    EXPECT_FALSE(client_id_from_text("01JD7Q0M9WKE6Q3N2F4H8ZT5XI").has_value());
    EXPECT_TRUE(client_id_text(ulid("SHORT")).empty());
}

// ------------------------------------------------------------- signing ----

// Not a live key: a fixed pair whose signatures were computed independently
// with `openssl dgst -sha256 -hmac`, so a change to the canonical string or to
// the digest shows up here rather than as a revoked key in production.
constexpr std::string_view k_key = "bingx-test-key";
constexpr std::string_view k_secret = "bingx-test-secret";
constexpr UnixNanos k_now{1'700'000'000'000ULL * 1'000'000ULL};

// The ledger cursor the income fixture's rows sit after.
constexpr UnixNanos k_since{1'702'713'600'000ULL * 1'000'000ULL};

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_key);
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
}

exec::InstrumentTable spot_catalogue() {
    static thread_local SpotSymbolBuffer storage;
    const auto body = load("rest_spot_symbols.json");
    const auto rows = parse_spot_symbols(body, storage);
    EXPECT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    for (const auto& row : *rows) {
        if (const auto instrument = map_spot_instrument(row); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    return table;
}

// ------------------------------------------------- spot market data ----

TEST(BingxSpotWire, TheSpotBookArrivesWithItsBestOfferLast) {
    // The whole reason parse_depth takes a market. BingX sends spot asks
    // DESCENDING while bids stay descending, so reading the spot tree the swap
    // way puts the worst offer at the top of the book — crossed against its own
    // bid, by a hundred dollars on BTC, and every strategy acts on that.
    LevelBuffer storage;
    const auto frame = load("ws_spot_depth20.json");

    const auto wrong = parse_depth(frame, storage, MarketType::Perp);
    ASSERT_TRUE(wrong.has_value());
    ASSERT_GE(wrong->asks.size(), 2U);
    EXPECT_GT(wrong->asks[0].price, wrong->asks[1].price) << "the wire really is worst-first";

    const auto right = parse_depth(frame, storage, MarketType::Spot);
    ASSERT_TRUE(right.has_value());
    ASSERT_GE(right->asks.size(), 2U);
    EXPECT_LT(right->asks[0].price, right->asks[1].price);
    EXPECT_GT(right->bids[0].price, right->bids[1].price);
    EXPECT_LT(right->bids[0].price, right->asks[0].price) << "an uncrossed book";
}

TEST(BingxSpotWire, ASingleTradeObjectReadsLikeAnArrayOfOne) {
    // The swap tree sends an array under "data"; spot sends one object. Both
    // reach the same span, so nothing downstream knows which socket it is on.
    TradeBuffer storage;
    // Bound, not inlined: the parsers return views INTO this text.
    const auto frame = load("ws_spot_trade.json");
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].symbol, "BTC-USDT");
    EXPECT_FALSE((*trades)[0].price.empty());
    EXPECT_NE((*trades)[0].trade_ms, 0U);
}

TEST(BingxSpotWire, TheSpotQuoteCarriesNoSequenceAndSaysSo) {
    // Spot omits u and T entirely. Zero here means ABSENT — the swap tree's
    // parse would have refused the frame outright if these were required.
    const auto frame = load("ws_spot_book_ticker.json");
    const auto quote = parse_book_ticker(frame);
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->symbol, "BTC-USDT");
    EXPECT_EQ(quote->update_id, 0U);
    EXPECT_EQ(quote->transaction_ms, 0U);
    EXPECT_NE(quote->event_ms, 0U);
}

TEST(BingxSpotWire, TheSpotTickerSendsTheSameColumnsUnquoted) {
    // The perpetual tree quotes `v` and `q`; spot sends them as bare JSON
    // numbers. One scalar reader covers both, and a string-only read would
    // drop every spot turnover while the swap kept working.
    const auto frame = load("ws_spot_ticker.json");
    const auto wire = parse_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, "BTC-USDT");
    EXPECT_EQ(wire->volume, "3150.99");
    EXPECT_EQ(wire->quote_volume, "254483206.58");

    const auto table = spot_catalogue();
    const auto stats = map_ticker_stats(*wire, table, MarketType::Spot, UnixNanos{5});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->instrument.market(), MarketType::Spot);
    // 254483206.58 / 3150.99 = 80762, inside the frame's 79000-82280.99 band.
    EXPECT_EQ(stats->volume_24h_base.raw, 3'150'990'000'000LL);
    EXPECT_EQ(stats->volume_24h_quote.raw, 254'483'206'580'000'000LL);
}

TEST(BingxSpotMap, ASpotFrameResolvesAgainstTheSpotCatalogue) {
    const auto table = spot_catalogue();
    const auto frame = load("ws_spot_book_ticker.json");
    const auto quote = parse_book_ticker(frame);
    ASSERT_TRUE(quote.has_value());

    // Asked for the perpetual, the same frame resolves to nothing: BingX
    // spells both markets "BTC-USDT", so the market is the only thing keeping
    // a spot print off a perpetual instrument.
    EXPECT_FALSE(map_bbo(*quote, table, MarketType::Perp, UnixNanos{1}).has_value());

    const auto bbo = map_bbo(*quote, table, MarketType::Spot, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument.market(), MarketType::Spot);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
}

OrderCmd limit_buy(const exec::InstrumentTable& table,
                   std::string_view symbol,
                   std::int64_t ticks,
                   std::int64_t lots) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = base_for(table, symbol).id;
    cmd.exchange_client_order_id = ulid();
    cmd.price = PriceTicks::from_raw(ticks);
    cmd.qty = QtyLots::from_raw(lots);
    return cmd;
}

// Everything after the '?' and before the signature: the bytes that were
// hashed, which must also be the bytes that were sent.
std::string_view query_of(std::string_view target) {
    const auto at = target.find('?');
    return at == std::string_view::npos ? std::string_view{} : target.substr(at + 1);
}

std::vector<std::string_view> keys_of(std::string_view canonical) {
    std::vector<std::string_view> keys;
    while (!canonical.empty()) {
        const auto amp = canonical.find('&');
        const auto pair = canonical.substr(0, amp);
        keys.push_back(pair.substr(0, pair.find('=')));
        if (amp == std::string_view::npos) {
            break;
        }
        canonical.remove_prefix(amp + 1);
    }
    return keys;
}

TEST(BingxSign, ReproducesAnIndependentlyComputedVector) {
    // BingX's own documented example parameter string, signed with the key its
    // documentation names. The expected digest came from openssl, not from
    // this code, so it pins the primitive and not merely our own opinion.
    std::array<char, net::k_hex_bytes> out{};
    const auto hex = net::hmac_sha256_hex(
        "SECRET_KEY", "recvWindow=0&symbol=BTC-USDT&timestamp=1696751141337", out);
    EXPECT_EQ(hex, "fe041f159118c90ac13eab4d32f9e2d75b80ca6fe17ca8acd290aba864753ce2");
}

TEST(BingxSign, PerpetualCreateSignsTheStringItSends) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request =
        builder.create_order(limit_buy(table, "BTC-USDT", 444'596, 30), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    constexpr std::string_view k_canonical =
        "clientOrderID=01JD7Q0M9WKE6Q3N2F4H8ZT5XR&clientOrderId=01JD7Q0M9WKE6Q3N2F4H8ZT5XR"
        "&positionSide=BOTH&price=44459.6&quantity=0.003&recvWindow=5000&side=BUY"
        "&symbol=BTC-USDT&timeInForce=GTC&timestamp=1699999999500&type=LIMIT";
    EXPECT_EQ(builder.last_sign_message(), k_canonical);
    // The same bytes on the wire: the server verifies the query string it
    // received, so rebuilding it separately is the edit that revokes a key.
    EXPECT_EQ(query_of(request->target).substr(0, k_canonical.size()), k_canonical);
    EXPECT_EQ(request->target,
              std::string{"/openApi/swap/v2/trade/order?"} + std::string{k_canonical} +
                  "&signature=4a5476229548e75de93fce60a00e998fd2dbcb83c8eed681f232ea46ead4e068");
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_TRUE(request->body.empty()); // parameters ride the query, not a body
    ASSERT_EQ(request->headers.size(), 1U);
    EXPECT_EQ(request->headers[0].name, "X-BX-APIKEY");
    EXPECT_EQ(request->headers[0].value, k_key);
}

TEST(BingxSign, SpotCreateSignsItsOwnCanonicalString) {
    const auto table = spot_catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    // The same order on the other tree: a different client-id field, no
    // positionSide, and therefore a different digest for the same intent.
    const auto request =
        builder.create_order(limit_buy(table, "BTC-USDT", 4'445'960, 3'000), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    constexpr std::string_view k_canonical =
        "newClientOrderId=01JD7Q0M9WKE6Q3N2F4H8ZT5XR&price=44459.6&quantity=0.003"
        "&recvWindow=5000&side=BUY&symbol=BTC-USDT&timeInForce=GTC"
        "&timestamp=1699999999500&type=LIMIT";
    EXPECT_EQ(builder.last_sign_message(), k_canonical);
    EXPECT_EQ(request->target,
              std::string{"/openApi/spot/v1/trade/order?"} + std::string{k_canonical} +
                  "&signature=774b96e3aa2d2ea8e123d674857108593feecbaca98c14811f0c00643c992054");
    EXPECT_EQ(request->method, net::HttpMethod::Post);
}

TEST(BingxSign, EveryCanonicalStringIsAsciiSorted) {
    // BingX verifies the string it received in the order it received it, so an
    // unsorted one still authenticates — but the documented convention is the
    // sort, and a field inserted in the wrong place is the kind of edit that
    // only shows up as a signature error on one endpoint.
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto check = [&](std::string_view what) {
        const auto keys = keys_of(builder.last_sign_message());
        EXPECT_TRUE(std::ranges::is_sorted(keys)) << what << ": " << builder.last_sign_message();
    };

    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    check("create");
    cmd.exchange_client_order_id = ulid();
    ASSERT_TRUE(builder.cancel_order(cmd, table, k_now).has_value());
    check("cancel");
    cmd.price = PriceTicks{};
    ASSERT_TRUE(builder.amend_order(cmd, table, k_now).has_value());
    check("amend");
    ASSERT_TRUE(builder.open_orders(MarketType::Perp, k_now).has_value());
    check("openOrders");
    ASSERT_TRUE(builder.positions(MarketType::Perp, k_now).has_value());
    check("positions");
    ASSERT_TRUE(builder.position_mode(k_now).has_value());
    check("positionSide/dual");
    ASSERT_TRUE(builder.balance(k_now).has_value());
    check("balance");
    ASSERT_TRUE(builder.funding_income(k_since, k_now).has_value());
    check("income");
    ASSERT_TRUE(builder.listen_key_keepalive("abc123", k_now).has_value());
    check("listenKey");
}

TEST(BingxSign, AsksTheWalletAndTheBookOnTheSwapTree) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto wallet = builder.balance(k_now);
    ASSERT_TRUE(wallet.has_value());
    EXPECT_EQ(wallet->method, net::HttpMethod::Get);
    // v3 for the wallet and v2 for the book, which is BingX's own spelling:
    // v3 states every settlement asset where v2 states one.
    EXPECT_TRUE(wallet->target.starts_with("/openApi/swap/v3/user/balance?"));

    const auto book = builder.funding_income(k_since, k_now);
    ASSERT_TRUE(book.has_value());
    EXPECT_TRUE(book->target.starts_with("/openApi/swap/v2/user/income?"));
    EXPECT_NE(builder.last_sign_message().find("startTime=1702713600000"), std::string_view::npos);
    // No incomeType filter: the same call states TRADING_FEE beside
    // FUNDING_FEE, and asking twice would read the two halves at two instants.
    EXPECT_EQ(builder.last_sign_message().find("incomeType="), std::string_view::npos);

    // A cursor of zero is no cursor at all, and startTime=0 would ask BingX
    // for the whole retained window on every pass.
    ASSERT_TRUE(builder.funding_income(UnixNanos{}, k_now).has_value());
    EXPECT_EQ(builder.last_sign_message().find("startTime="), std::string_view::npos);
}

// -------------------------------------------------------- request shape ----

TEST(BingxSign, RoutesEachMarketToItsOwnTree) {
    const auto perp = catalogue();
    const auto spot = spot_catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cancel = limit_buy(perp, "BTC-USDT", 444'596, 30);
    cancel.kind = CmdKind::Cancel;
    const auto perp_cancel = builder.cancel_order(cancel, perp, k_now);
    ASSERT_TRUE(perp_cancel.has_value());
    EXPECT_EQ(perp_cancel->method, net::HttpMethod::Delete);
    EXPECT_TRUE(perp_cancel->target.starts_with("/openApi/swap/v2/trade/order?"));
    // Both spellings go out on the perpetual tree: BingX's parameter table and
    // its own worked example disagree, and an order placed under neither name
    // is one whose every later echo is unattributable.
    EXPECT_NE(builder.last_sign_message().find("clientOrderID=01JD"), std::string_view::npos);
    EXPECT_NE(builder.last_sign_message().find("clientOrderId=01JD"), std::string_view::npos);

    auto spot_cancel_cmd = limit_buy(spot, "BTC-USDT", 4'445'960, 3'000);
    spot_cancel_cmd.kind = CmdKind::Cancel;
    const auto spot_cancel = builder.cancel_order(spot_cancel_cmd, spot, k_now);
    ASSERT_TRUE(spot_cancel.has_value());
    // A POST, not a DELETE, and one spelling only — the trees differ by both.
    EXPECT_EQ(spot_cancel->method, net::HttpMethod::Post);
    EXPECT_TRUE(spot_cancel->target.starts_with("/openApi/spot/v1/trade/cancel?"));
    EXPECT_NE(builder.last_sign_message().find("clientOrderID=01JD"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("clientOrderId=01JD"), std::string_view::npos);

    ASSERT_TRUE(builder.open_orders(MarketType::Spot, k_now).has_value());
    EXPECT_TRUE(builder.open_orders(MarketType::Spot, k_now)
                    ->target.starts_with("/openApi/spot/v1/trade/openOrders?"));
    EXPECT_TRUE(builder.open_orders(MarketType::Perp, k_now)
                    ->target.starts_with("/openApi/swap/v2/trade/openOrders?"));
    EXPECT_TRUE(builder.positions(MarketType::Perp, k_now)
                    ->target.starts_with("/openApi/swap/v2/user/positions?"));
}

TEST(BingxSign, CancelsByVenueIdWhenItHasOne) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    cmd.kind = CmdKind::Cancel;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("1585839271162413056");
    ASSERT_TRUE(builder.cancel_order(cmd, table, k_now).has_value());
    // One target, never two: an orderId and a clientOrderId that disagree name
    // two different orders.
    EXPECT_NE(builder.last_sign_message().find("orderId=1585839271162413056"),
              std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("clientOrderId="), std::string_view::npos);

    // A cancel that knows neither has nothing to name.
    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
}

TEST(BingxSign, HedgeModeChangesThePositionSideAndDropsReduceOnly) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    EXPECT_FALSE(builder.hedge_mode()); // BingX's own default

    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::ReduceOnly;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find("positionSide=BOTH"), std::string_view::npos);
    EXPECT_NE(builder.last_sign_message().find("reduceOnly=true"), std::string_view::npos);

    builder.set_hedge_mode(true);
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    // A SELL that closes a long carries positionSide LONG. Reading the side as
    // the position side opens the opposite position instead.
    EXPECT_NE(builder.last_sign_message().find("positionSide=LONG"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("reduceOnly"), std::string_view::npos);

    cmd.flags = OrderFlags::None;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find("positionSide=SHORT"), std::string_view::npos);
}

TEST(BingxSign, PostOnlyIsATimeInForceAndAMarketOrderCarriesNoPrice) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    cmd.flags = OrderFlags::PostOnly;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find("timeInForce=PostOnly"), std::string_view::npos);

    cmd.flags = OrderFlags::None;
    cmd.order_type = OrderType::Market;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find("type=MARKET"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("price="), std::string_view::npos);
    EXPECT_EQ(time_in_force(TimeInForce::Ioc, OrderFlags::None), "IOC");
    EXPECT_EQ(time_in_force(TimeInForce::Fok, OrderFlags::None), "FOK");
}

TEST(BingxSign, RefusesUnservedMarketsBeforeReadingTheTable) {
    // The table resolves an id by its WHOLE value, so an id for a market this
    // adapter never loaded finds nothing and would otherwise be reported as a
    // stale catalogue rather than as a market BingX does not serve here.
    EXPECT_FALSE(market_refusal(MarketType::Spot).has_value());
    EXPECT_FALSE(market_refusal(MarketType::Perp).has_value());
    EXPECT_EQ(market_refusal(MarketType::Future), BuildError::DatedFuturesUnsupported);
    EXPECT_EQ(market_refusal(MarketType::Option), BuildError::OptionsUnsupported);

    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    OrderCmd cmd{};
    cmd.instrument = InstrumentId::make(VenueId::Bingx, MarketType::Option, SymbolId{0});
    cmd.exchange_client_order_id = ulid();
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.cancel_order(cmd, table, k_now).error(), BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.amend_order(cmd, table, k_now).error(), BuildError::OptionsUnsupported);
    cmd.instrument = InstrumentId::make(VenueId::Bingx, MarketType::Future, SymbolId{0});
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::DatedFuturesUnsupported);
    EXPECT_EQ(builder.positions(MarketType::Option, k_now).error(), BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.positions(MarketType::Spot, k_now).error(),
              BuildError::SpotPositionsUnsupported);
}

TEST(BingxSign, RefusesAnAmendItCannotExpress) {
    const auto perp = catalogue();
    const auto spot = spot_catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    // /openApi/swap/v1/trade/amend takes a quantity and no price at all, so an
    // amend that restates one is refused rather than sent as a size change
    // that silently leaves the price where it was.
    auto cmd = limit_buy(perp, "BTC-USDT", 444'596, 30);
    cmd.kind = CmdKind::Amend;
    EXPECT_EQ(builder.amend_order(cmd, perp, k_now).error(), BuildError::AmendPriceUnsupported);

    cmd.price = PriceTicks{};
    const auto sized = builder.amend_order(cmd, perp, k_now);
    ASSERT_TRUE(sized.has_value());
    EXPECT_TRUE(sized->target.starts_with("/openApi/swap/v1/trade/amend?"));
    EXPECT_NE(builder.last_sign_message().find("quantity=0.003"), std::string_view::npos);

    auto spot_cmd = limit_buy(spot, "BTC-USDT", 4'445'960, 3'000);
    spot_cmd.kind = CmdKind::Amend;
    spot_cmd.price = PriceTicks{};
    EXPECT_EQ(builder.amend_order(spot_cmd, spot, k_now).error(), BuildError::SpotAmendUnsupported);
}

TEST(BingxSign, RefusesWhatItCannotSignHonestly) {
    const auto table = catalogue();
    net::Signer unloaded;
    SignedRequestBuilder anonymous{unloaded};
    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    EXPECT_EQ(anonymous.create_order(cmd, table, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(anonymous.open_orders(MarketType::Perp, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(anonymous.listen_key_create(k_now).error(), BuildError::NotSigned);

    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    // An id that is not a ULID has no shape this adapter can send, and a zero
    // quantity has no order behind it.
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("webOrder1");
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
    cmd.exchange_client_order_id = ulid();
    cmd.qty = QtyLots::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::BadQuantity);
    cmd.qty = QtyLots::from_raw(30);
    cmd.price = PriceTicks::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::BadPrice);

    // A key carrying a query separator would become two signed parameters.
    EXPECT_EQ(builder.listen_key_keepalive("abc&symbol=ETH-USDT", k_now).error(),
              BuildError::UnsafeParameter);
}

TEST(BingxSign, ManagesTheListenKeyOverThreeVerbs) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto mint = builder.listen_key_create(k_now);
    ASSERT_TRUE(mint.has_value());
    EXPECT_EQ(mint->method, net::HttpMethod::Post);
    EXPECT_TRUE(mint->target.starts_with("/openApi/user/auth/userDataStream?"));
    EXPECT_EQ(builder.listen_key_keepalive("abc123", k_now)->method, net::HttpMethod::Put);
    EXPECT_EQ(builder.listen_key_close("abc123", k_now)->method, net::HttpMethod::Delete);
    EXPECT_NE(builder.last_sign_message().find("listenKey=abc123"), std::string_view::npos);
}

TEST(BingxWs, NamesTheUserStreamPerTree) {
    // The perpetual user stream is the public market socket with a key on the
    // query; spot is a different deployment that also needs a subscribe.
    EXPECT_EQ(private_stream_host(MarketType::Perp), "open-api-swap.bingx.com");
    EXPECT_EQ(private_stream_host(MarketType::Spot), "open-api-ws.bingx.com");
    EXPECT_EQ(private_stream_target(MarketType::Perp, "abc"), "/swap-market?listenKey=abc");
    EXPECT_EQ(private_stream_target(MarketType::Spot, "abc"), "/market?listenKey=abc");

    Protocol protocol;
    const auto frame = protocol.subscribe_channel("spot.executionReport");
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"id":"1","reqType":"sub","dataType":"spot.executionReport"})");
}

// -------------------------------------------------------- private wire ----

TEST(BingxWire, ReadsTheSpotCatalogue) {
    const auto body = load("rest_spot_symbols.json");
    static thread_local SpotSymbolBuffer storage;
    const auto rows = parse_spot_symbols(body, storage);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_EQ(rows->size(), 5U);
    EXPECT_EQ((*rows)[0].symbol, "BTC-USDT");
    EXPECT_EQ((*rows)[0].tick_size, "0.01");
    EXPECT_EQ((*rows)[0].step_size, "0.000001");
    EXPECT_EQ((*rows)[0].min_notional, "0.5");
    EXPECT_EQ((*rows)[0].status, 1);
    EXPECT_TRUE((*rows)[0].api_buy);
    // Scientific notation off the live wire; parse_decimal_e9 cannot read it
    // and parse_number_e9 must.
    EXPECT_EQ((*rows)[2].tick_size, "1e-10");
    EXPECT_FALSE((*rows)[3].api_buy);
    EXPECT_EQ((*rows)[4].status, 0);
}

TEST(BingxWire, ReadsOpenOrdersOnBothTrees) {
    const auto perp_body = load("rest_open_orders_perp.json");
    static thread_local OrderBuffer storage;
    const auto perp = parse_orders(perp_body, storage);
    ASSERT_TRUE(perp.has_value()) << to_string(perp.error());
    ASSERT_EQ(perp->size(), 2U);
    EXPECT_EQ((*perp)[0].symbol, "BTC-USDT");
    EXPECT_EQ((*perp)[0].order_id, "1585839271162413056");
    EXPECT_EQ((*perp)[0].client_order_id, k_ulid_echo);
    // PENDING is a working order, not an unknown state.
    EXPECT_EQ((*perp)[0].status, WireOrderStatus::New);
    EXPECT_EQ((*perp)[0].price, "44459.6");
    EXPECT_EQ((*perp)[1].status, WireOrderStatus::PartiallyFilled);
    EXPECT_EQ((*perp)[1].executed_qty, "0.40");

    const auto spot_body = load("rest_open_orders_spot.json");
    static thread_local OrderBuffer spot_storage;
    const auto spot = parse_orders(spot_body, spot_storage);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    ASSERT_EQ(spot->size(), 1U);
    // Spot spells the field clientOrderID; one reader takes either.
    EXPECT_EQ((*spot)[0].client_order_id, k_ulid_spot);
    EXPECT_EQ((*spot)[0].status, WireOrderStatus::New);
}

TEST(BingxWire, ReadsAnAckWhicheverWayItIsWrapped) {
    // The perpetual tree nests the order one level deeper than spot does, and
    // an ack read from the wrong depth seeds no memo at all.
    const auto perp_body = load("rest_order_ack_perp.json");
    const auto perp = parse_order_ack(perp_body);
    ASSERT_TRUE(perp.has_value()) << to_string(perp.error());
    EXPECT_EQ(perp->order_id, "1709036527545438208");
    EXPECT_EQ(perp->client_order_id, k_ulid_echo);

    const auto spot_body = load("rest_order_ack_spot.json");
    const auto spot = parse_order_ack(spot_body);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    EXPECT_EQ(spot->order_id, "1514090846268424192");
    EXPECT_EQ(spot->symbol, "BTC-USDT");
}

TEST(BingxWire, ReadsPositionsAndTheListenKeyAndTheMode) {
    const auto positions_body = load("rest_positions.json");
    static thread_local PositionBuffer storage;
    const auto positions = parse_positions(positions_body, storage);
    ASSERT_TRUE(positions.has_value()) << to_string(positions.error());
    ASSERT_EQ(positions->size(), 2U);
    EXPECT_EQ((*positions)[0].position_amt, "0.0314");
    EXPECT_EQ((*positions)[1].position_side, "SHORT");

    // BingX documents this answer both bare and wrapped; either one has to
    // connect the user stream.
    const auto bare = load("rest_listen_key.json");
    const auto wrapped = load("rest_listen_key_wrapped.json");
    ASSERT_TRUE(parse_listen_key(bare).has_value());
    ASSERT_TRUE(parse_listen_key(wrapped).has_value());
    EXPECT_EQ(*parse_listen_key(bare), *parse_listen_key(wrapped));

    const auto mode_body = load("rest_position_mode.json");
    const auto mode = parse_position_mode(mode_body);
    ASSERT_TRUE(mode.has_value());
    EXPECT_TRUE(*mode); // "true", a STRING, not a JSON bool
}

TEST(BingxWire, SortsPrivateFramesByWhatTheyAre) {
    const auto fill = load("ws_order_update_perp_fill.json");
    const auto spot_fill = load("ws_order_update_spot_fill.json");
    const auto expired = load("ws_listen_key_expired.json");
    const auto ack = load("ws_subscribe_ack.json");
    const auto refusal = load("ws_error.json");
    EXPECT_EQ(*parse_private_event(fill), PrivateKind::OrderUpdate);
    EXPECT_EQ(*parse_private_event(spot_fill), PrivateKind::OrderUpdate);
    EXPECT_EQ(*parse_private_event(expired), PrivateKind::ListenKeyExpired);
    EXPECT_EQ(*parse_private_event(ack), PrivateKind::Ack);
    EXPECT_EQ(*parse_private_event(refusal), PrivateKind::Error);
}

TEST(BingxWire, ReadsAnOrderUpdateFromEitherTree) {
    // The perpetual push quotes every number and spot sends them bare; one
    // scalar reader takes both, and reading either as the other loses the field.
    const auto perp_body = load("ws_order_update_perp_fill.json");
    const auto perp = parse_order_update(perp_body);
    ASSERT_TRUE(perp.has_value()) << to_string(perp.error());
    EXPECT_EQ(perp->symbol, "BTC-USDT");
    EXPECT_EQ(perp->client_order_id, k_ulid_echo);
    EXPECT_EQ(perp->exec_type, "TRADE");
    EXPECT_EQ(perp->last_qty, "0.0012");
    EXPECT_EQ(perp->last_price, "44459.6");
    EXPECT_EQ(perp->fee, "-0.02667576");
    EXPECT_FALSE(perp->maker_known); // the perpetual push says nothing about it
    EXPECT_TRUE(perp->trade_id.empty());

    const auto spot_body = load("ws_order_update_spot_fill.json");
    const auto spot = parse_order_update(spot_body);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    EXPECT_EQ(spot->symbol, "BTC-USDT");
    EXPECT_EQ(spot->last_qty, "0.005");
    EXPECT_EQ(spot->last_price, "44647");
    EXPECT_EQ(spot->trade_id, "83956769");
    EXPECT_TRUE(spot->maker_known);
    EXPECT_FALSE(spot->is_maker);
    EXPECT_TRUE(spot->client_order_id.empty()); // which is what the memo is for
}

TEST(BingxWire, ReadsBothSpellingsOfCancelled) {
    EXPECT_EQ(order_status_of("CANCELED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status_of("CANCELLED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status_of("Pending"), WireOrderStatus::New);
    EXPECT_EQ(order_status_of("FAILED"), WireOrderStatus::Rejected);
    EXPECT_EQ(order_status_of("Unknown"), WireOrderStatus::Unknown);
    EXPECT_EQ(order_status_of(""), WireOrderStatus::Unknown);
}

// --------------------------------------------------------- private map ----

TEST(BingxMap, BuildsTheSpotCatalogueFromStatedSteps) {
    const auto table = spot_catalogue();
    // Two of the five rows survive: one is offline, one is closed to the API,
    // and one quotes a tick of 1e-10 that FixedE9 cannot hold exactly.
    EXPECT_EQ(table.size(), 2U);
    const auto& btc = base_for(table, "BTC-USDT");
    EXPECT_EQ(btc.id.market(), MarketType::Spot);
    EXPECT_EQ(btc.tick_size.raw, 10'000'000); // 0.01
    EXPECT_EQ(btc.lot_size.raw, 1'000);       // 1e-6, written "0.000001"
    EXPECT_EQ(btc.min_notional.raw, 500'000'000);
    EXPECT_EQ(btc.contract_value.raw, k_e9); // spot sizes are base units
    EXPECT_EQ(btc.kind, ContractKind::Linear);
}

TEST(BingxMap, ReportsOpenOrdersWithTheirLeaves) {
    const auto table = catalogue();
    const auto body = load("rest_open_orders_perp.json");
    static thread_local OrderBuffer storage;
    const auto rows = parse_orders(body, storage);
    ASSERT_TRUE(rows.has_value());

    const auto resting = map_open_order((*rows)[0], table, MarketType::Perp);
    ASSERT_TRUE(resting.has_value());
    EXPECT_EQ(resting->status, OrderStatus::Accepted);
    EXPECT_EQ(resting->side, Side::Sell);
    EXPECT_EQ(resting->price.raw, 444'596);
    EXPECT_EQ(resting->qty.raw, 30);
    EXPECT_EQ(resting->leaves.raw, 30);
    // The echo came back lowercased and comes out as the id we minted.
    EXPECT_EQ(resting->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(resting->exchange_order_id.view(), "1585839271162413056");

    const auto partial = map_open_order((*rows)[1], table, MarketType::Perp);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(partial->qty.raw, 100);   // 1.00 at a lot of 0.01
    EXPECT_EQ(partial->leaves.raw, 60); // 1.00 - 0.40
}

TEST(BingxMap, RefusesARowFromTheOtherMarket) {
    // BingX spells a spot pair and a perpetual identically, and the table holds
    // whichever loaded first. A perpetual row read as spot must resolve to
    // nothing rather than to the same symbol under the wrong market.
    const auto table = catalogue();
    const auto body = load("rest_open_orders_perp.json");
    static thread_local OrderBuffer storage;
    const auto rows = parse_orders(body, storage);
    ASSERT_TRUE(rows.has_value());
    EXPECT_FALSE(map_open_order((*rows)[0], table, MarketType::Spot).has_value());
}

TEST(BingxMap, MapsThePerpetualLifecycle) {
    const auto table = catalogue();

    const auto accepted_body = load("ws_order_update_perp_new.json");
    const auto accepted_wire = parse_order_update(accepted_body);
    ASSERT_TRUE(accepted_wire.has_value());
    const auto accepted = map_order_update(
        *accepted_wire, table, MarketType::Perp, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->qty.raw, 30);
    EXPECT_EQ(accepted->leaves_qty.raw, 30);
    EXPECT_EQ(accepted->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(accepted->ts_event.ns, 1'760'079'163'280ULL * 1'000'000ULL);

    const auto fill_body = load("ws_order_update_perp_fill.json");
    const auto fill_wire = parse_order_update(fill_body);
    ASSERT_TRUE(fill_wire.has_value());
    const auto fill = map_order_update(
        *fill_wire, table, MarketType::Perp, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    // The fill states its OWN size and price, not the order's and not the
    // average: 0.0012 at 44459.6 out of 0.0030.
    EXPECT_EQ(fill->qty.raw, 12);
    EXPECT_EQ(fill->price.raw, 444'596);
    EXPECT_EQ(fill->leaves_qty.raw, 18);
    // The wire says "-0.02667576" and BingX means CHARGED by that minus, like
    // okx and bitget. Flipped on the way in, because every consumer here
    // subtracts a positive fee from realized PnL.
    EXPECT_EQ(fill->fee.raw, 26'675'760);
    EXPECT_EQ(fill->liquidity, Liquidity::Unknown); // the push does not say
    EXPECT_TRUE(fill->exec_id.empty());             // and mints no trade id

    const auto cancel_body = load("ws_order_update_perp_cancel.json");
    const auto cancel_wire = parse_order_update(cancel_body);
    ASSERT_TRUE(cancel_wire.has_value());
    const auto cancel = map_order_update(
        *cancel_wire, table, MarketType::Perp, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->type, OrderEventType::Canceled);
    EXPECT_EQ(cancel->leaves_qty.raw, 18);
}

TEST(BingxMap, MapsASpotFillThroughTheMemo) {
    const auto table = spot_catalogue();
    const auto body = load("ws_order_update_spot_fill.json");
    const auto wire = parse_order_update(body);
    ASSERT_TRUE(wire.has_value());

    // The spot push names no client order id, so without the memo the fill is
    // unattributable — which is the whole reason the memo exists.
    const auto orphan =
        map_order_update(*wire, table, MarketType::Spot, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(orphan.has_value());
    EXPECT_TRUE(orphan->exchange_client_order_id.empty());

    const auto event = map_order_update(*wire, table, MarketType::Spot, ulid(), UnixNanos{7});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(event->qty.raw, 5'000); // 0.005 at a step of 1e-6
    EXPECT_EQ(event->price.raw, 4'464'700);
    EXPECT_EQ(event->leaves_qty.raw, 0);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->exec_id.view(), "83956769");
    EXPECT_EQ(event->fee.raw, 5'000); // charged, sign flipped on the way in
}

TEST(BingxMap, SignsAPositionByItsSide) {
    const auto table = catalogue();
    const auto body = load("rest_positions.json");
    static thread_local PositionBuffer storage;
    const auto rows = parse_positions(body, storage);
    ASSERT_TRUE(rows.has_value());

    const auto long_side = map_position((*rows)[0], table);
    ASSERT_TRUE(long_side.has_value());
    EXPECT_EQ(long_side->net_qty.raw, 314); // 0.0314 at a lot of 0.0001
    EXPECT_EQ(long_side->avg_entry.raw, 755'860);

    // positionAmt is unsigned on both sides, so positionSide is the only thing
    // that says which way the position points.
    const auto short_side = map_position((*rows)[1], table);
    ASSERT_TRUE(short_side.has_value());
    EXPECT_EQ(short_side->net_qty.raw, -20);
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

    // The key, the signature and the account's standing all halt the venue:
    // retrying a permission refusal at full speed is how an IP earns a ban.
    EXPECT_EQ(classify(k_bad_signature), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_missing_signature), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_no_permission), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_ip_not_whitelisted), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_kyc_required), exec::ErrorClass::AuthFailure);

    EXPECT_EQ(classify(k_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_risk_control), exec::ErrorClass::RateLimited);

    // Clock skew is fixed by resyncing, not by halting a venue whose keys are
    // fine — so the timestamp-window refusal is Retryable and NOT AuthFailure.
    EXPECT_EQ(classify(k_bad_timestamp), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_funding_settling), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_order_system_busy), exec::ErrorClass::Retryable);
}

// ------------------------------------------- the wallet and the book ----

TEST(BingxWire, ReadsTheMarginWalletAndTheAccountBook) {
    const auto wallet_body = load("rest_balance.json");
    static thread_local BalanceBuffer wallet;
    const auto balances = parse_balances(wallet_body, wallet);
    ASSERT_TRUE(balances.has_value()) << to_string(balances.error());
    ASSERT_EQ(balances->size(), 2U);
    EXPECT_EQ((*balances)[0].asset, "USDT");
    EXPECT_EQ((*balances)[0].balance, "194.8212");
    EXPECT_EQ((*balances)[0].available_margin, "193.7609");
    EXPECT_EQ((*balances)[0].unrealized_profit, "1.9219");
    EXPECT_EQ((*balances)[1].asset, "USDC");

    const auto book_body = load("rest_income.json");
    static thread_local IncomeBuffer book;
    const auto rows = parse_income(book_body, book);
    ASSERT_TRUE(rows.has_value()) << to_string(rows.error());
    ASSERT_EQ(rows->size(), 3U);
    EXPECT_EQ((*rows)[0].income_type, "FUNDING_FEE");
    // NEGATIVE, and the minus is the whole point: the account paid this one.
    EXPECT_EQ((*rows)[0].income, "-0.0292");
    EXPECT_EQ((*rows)[0].time_ms, 1'702'713'615'000ULL);
    // Underscored and masked: neither id reads as a number.
    EXPECT_EQ((*rows)[0].tran_id, "170***6*2_3*9_20***97");
    EXPECT_EQ((*rows)[1].income_type, "TRADING_FEE");
    EXPECT_EQ((*rows)[1].trade_id, "83956769");
}

TEST(BingxMap, BalanceKeepsEveryFigureBingxStated) {
    const auto body = load("rest_balance.json");
    static thread_local BalanceBuffer wallet;
    const auto balances = parse_balances(body, wallet);
    ASSERT_TRUE(balances.has_value());
    const auto report = map_balance((*balances)[0], k_now);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->asset.view(), "USDT");
    EXPECT_EQ(report->total.raw, 194'821'200'000);
    // Its own per-asset free figure, not an account-wide one repeated: the
    // margin already committed is in usedMargin and freezedMargin.
    EXPECT_EQ(report->available.raw, 193'760'900'000);
    EXPECT_EQ(report->unrealized.raw, 1'921'900'000);
    EXPECT_EQ(report->ts_venue.ns, k_now.ns);
    // `equity` is 196.7431 — balance plus the open pnl — and is read nowhere:
    // publishing it as the total would count that pnl twice.
    EXPECT_NE(report->total.raw, 196'743'100'000);
}

TEST(BingxMap, BalanceLeavesAFieldBingxDidNotSendUndef) {
    WireBalance wire{};
    wire.asset = "USDT";
    wire.balance = "1200.0000";
    const auto report = map_balance(wire, k_now);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->total.raw, 1'200'000'000'000);
    // Not zero, and not the total: repeating either invites a caller to spend
    // margin that is already committed.
    EXPECT_TRUE(report->available.is_undef());
    EXPECT_TRUE(report->unrealized.is_undef());
}

TEST(BingxMap, BalanceWithoutAnAssetIsDropped) {
    // A wallet figure with no unit is not a balance, and a truncated ticker
    // would attribute the money to an asset that does not exist.
    WireBalance wire{};
    wire.balance = "12.5000";
    EXPECT_FALSE(map_balance(wire, k_now).has_value());
    wire.asset = "TWENTYONECHARACTERSXX";
    EXPECT_FALSE(map_balance(wire, k_now).has_value());
}

TEST(BingxErrors, ReadsARefusalOutOfABodyThatCameBackWithHttp200) {
    // The captured proof: an unauthenticated private call answers HTTP 200
    // with 100413 in the body. Reading the status instead of the envelope
    // calls a revoked key a healthy request.
    const auto body = load("rest_error_bad_key.json");
    const auto envelope = parse_rest_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_bad_api_key);
    EXPECT_EQ(classify(envelope->code), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_http(200), exec::ErrorClass::Terminal); // the envelope decides
}

TEST(BingxErrors, ForgivesACancelThatLostItsRace) {
    // The order finished before the cancel landed. A CancelRejected for it
    // would ask the OMS to resume an order that no longer exists.
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
    EXPECT_TRUE(is_benign_cancel_failure(k_spot_order_not_found));
    EXPECT_FALSE(is_benign_cancel_failure(k_invalid_parameter));
    EXPECT_FALSE(is_benign_cancel_failure(k_bad_api_key));
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
