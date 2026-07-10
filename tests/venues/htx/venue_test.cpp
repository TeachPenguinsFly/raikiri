#include <raikiri/domain/asset_class.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/htx/account.hpp>
#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/venues/htx/htx_venue.hpp>
#include <raikiri/venues/htx/map.hpp>
#include <raikiri/venues/htx/rest.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::htx {
namespace {

std::string load_bytes(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The JSON fixtures carry a trailing newline so the files are well-formed
// text; the frames HTX sends do not.
std::string load(std::string_view name) {
    auto text = load_bytes(name);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// Every mappable row in the contracts fixture, numbered the way a live
// catalogue load numbers them.
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

TEST(HtxVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Htx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Htx);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(HtxVenue, OrderWithoutAnImplementationFailsLoudly) {
    // Silence would leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Htx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(HtxVenue, PullsRefuseRatherThanAnswerEmptyWithoutCredentials) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter =
        exec::make_venue({.venue = VenueId::Htx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    // AuthFailure, not Terminal: the pull is implemented and would work — what
    // is missing is a key. Terminal would say retrying cannot help, and send an
    // operator looking for a venue outage instead of at the config.
    //
    // What reconciliation needs is only that this REFUSES: an empty vector
    // would be read as "the venue holds nothing of ours".
    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::AuthFailure);
    EXPECT_FALSE(adapter->execution->position_reports(MarketType::Perp).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
}

// ------------------------------------------------------- decompression ----

TEST(HtxInflate, RestoresACompressedFrame) {
    // The whole venue rests on this: the socket delivers gzip, and this is the
    // only place it stops being gzip. The fixture is a dynamic-Huffman block,
    // which is what a frame carrying real quotes actually compresses to.
    net::Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_bbo.frame.gz"));
    ASSERT_TRUE(frame.has_value()) << to_string(frame.error());
    EXPECT_EQ(*frame, load("ws_bbo.json"));
}

TEST(HtxInflate, RestoresTheKeepalive) {
    // Twenty-two bytes compress to a FIXED-Huffman block, so the two gz
    // fixtures between them cover both block types HTX emits.
    net::Inflater inflater;
    const auto frame = inflater.inflate(load_bytes("ws_ping.frame.gz"));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, load("ws_ping.json"));
}

TEST(HtxInflate, ReusesOneStreamAcrossFrames) {
    // reset() between frames, or the second one decodes against the first's
    // window and produces plausible garbage.
    net::Inflater inflater;
    const auto bbo = load_bytes("ws_bbo.frame.gz");
    const auto ping = load_bytes("ws_ping.frame.gz");
    for (int i = 0; i < 3; ++i) {
        const auto first = inflater.inflate(bbo);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(*first, load("ws_bbo.json"));
        const auto second = inflater.inflate(ping);
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(*second, load("ws_ping.json"));
    }
}

TEST(HtxInflate, RefusesWhatIsNotGzip) {
    // If HTX ever sent plain text, treating it as a frame silently would be
    // worse than refusing: the parsers would see nothing and the venue would
    // look quiet rather than broken.
    net::Inflater inflater;
    const auto plain = inflater.inflate(load("ws_bbo.json"));
    ASSERT_FALSE(plain.has_value());
    EXPECT_EQ(plain.error(), net::InflateError::NotGzip);
    EXPECT_EQ(inflater.inflate("").error(), net::InflateError::Truncated);
}

TEST(HtxInflate, RefusesATruncatedFrame) {
    net::Inflater inflater;
    const auto whole = load_bytes("ws_bbo.frame.gz");
    ASSERT_FALSE(inflater.inflate(std::string_view{whole}.substr(0, 8)).has_value());
    const auto half = inflater.inflate(std::string_view{whole}.substr(0, whole.size() / 2));
    ASSERT_FALSE(half.has_value());
    EXPECT_EQ(half.error(), net::InflateError::Corrupt);
}

// ------------------------------------------------------------ ws wire ----

TEST(HtxWire, ClassifiesEveryTopicAcrossBothSockets) {
    struct Row {
        std::string_view fixture;
        EventKind kind;
        std::string_view code;
    };

    for (const auto& row : {Row{"ws_bbo.json", EventKind::Bbo, "BTC-USDT"},
                            Row{"ws_trade.json", EventKind::Trade, "BTC-USDT"},
                            Row{"ws_trades_batch.json", EventKind::Trade, "ETH-USDT"},
                            Row{"ws_depth_step0.json", EventKind::Depth, "BTC-USDT"},
                            // "detail" and "trade.detail" are two topics whose
                            // suffixes end in the same word.
                            Row{"ws_detail.json", EventKind::Detail, "BTC-USDT"},
                            Row{"ws_notify_funding.json", EventKind::Funding, "BTC-USDT"}}) {
        const auto frame = load(row.fixture);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << row.fixture;
        EXPECT_EQ(event->kind, row.kind) << row.fixture;
        // Recovered from `ch` or `topic` — the depth payload never names the
        // contract, and "trade.detail" carries a dot of its own that the split
        // must not mistake for the separator.
        EXPECT_EQ(event->contract_code, row.code) << row.fixture;
    }
}

TEST(HtxWire, ClassifiesBothKeepalivesAndKeepsTheirSpelling) {
    // The market socket writes a number and the notification socket a quoted
    // string. Carrying the raw token is what lets one echo answer both.
    const auto market = load("ws_ping.json");
    const auto market_event = parse_event(market);
    ASSERT_TRUE(market_event.has_value());
    EXPECT_EQ(market_event->kind, EventKind::MarketPing);
    EXPECT_EQ(market_event->keepalive_ts, "1786431113951");

    const auto notify = load("ws_notify_ping.json");
    const auto notify_event = parse_event(notify);
    ASSERT_TRUE(notify_event.has_value());
    EXPECT_EQ(notify_event->kind, EventKind::NotifyPing);
    EXPECT_EQ(notify_event->keepalive_ts, "\"1786431124111\"");
}

TEST(HtxWire, SeparatesAnAcknowledgementFromARefusal) {
    // Every frame is a named string: the parsers hand back views INTO it, so a
    // temporary here would dangle the fields under test.
    const auto ack_frame = load("ws_subscribe_ack.json");
    const auto ack = parse_event(ack_frame);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, EventKind::SubscribeAck);

    const auto frame = load("ws_error.json");
    const auto refusal = parse_event(frame);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->kind, EventKind::Error);
    const auto err = parse_stream_error(frame);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, k_ws_bad_request);
    EXPECT_EQ(err->msg, "invalid topic market.BTC-USDT.nosuch");

    // The notification socket answers a sub with the same shape twice over:
    // err-code 0 is the acknowledgement and anything else the refusal.
    const auto notify_ack_frame = load("ws_notify_sub_ack.json");
    const auto notify_ack = parse_event(notify_ack_frame);
    ASSERT_TRUE(notify_ack.has_value());
    EXPECT_EQ(notify_ack->kind, EventKind::SubscribeAck);
    EXPECT_EQ(notify_ack->contract_code, "BTC-USDT");

    const auto notify_bad_frame = load("ws_notify_error.json");
    const auto notify_bad = parse_event(notify_bad_frame);
    ASSERT_TRUE(notify_bad.has_value());
    EXPECT_EQ(notify_bad->kind, EventKind::Error);
    const auto notify_err = parse_stream_error(notify_bad_frame);
    ASSERT_TRUE(notify_err.has_value());
    EXPECT_EQ(notify_err->code, "2011"); // a NUMBER here, a word on the other socket
    EXPECT_EQ(notify_err->msg, "Contract doesn't exist.");
}

TEST(HtxWire, ParsesBbo) {
    const auto frame = load("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract_code, "BTC-USDT");
    EXPECT_EQ(wire->bid_price, "63920.1");
    EXPECT_EQ(wire->bid_qty, "1455"); // contracts, not base units
    EXPECT_EQ(wire->ask_price, "63920.2");
    EXPECT_EQ(wire->ask_qty, "4485");
    EXPECT_EQ(wire->ts_ms, 1786431108849ULL); // tick.ts, not the frame's 1786431108850
    EXPECT_EQ(wire->version, 100125839299953ULL);
}

TEST(HtxWire, ParsedViewsPointIntoTheCallerBuffer) {
    // The rebase contract: a view that pointed into simdjson's scratch would
    // dangle the moment the next frame arrived.
    const auto frame = load("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    const auto* begin = frame.data();
    const auto* end = frame.data() + frame.size();
    for (const auto field : {wire->contract_code, wire->bid_price, wire->ask_qty}) {
        EXPECT_GE(field.data(), begin);
        EXPECT_LE(field.data() + field.size(), end);
    }
}

TEST(HtxWire, ParsesTradesIncludingABatch) {
    TradeBuffer storage;
    const auto single = load("ws_trade.json");
    const auto one = parse_trades(single, storage);
    ASSERT_TRUE(one.has_value());
    ASSERT_EQ(one->size(), 1U);
    EXPECT_EQ((*one)[0].contract_code, "BTC-USDT");
    EXPECT_EQ((*one)[0].price, "63920.2");
    EXPECT_EQ((*one)[0].amount, "2");
    EXPECT_EQ((*one)[0].quantity, "0.002"); // the same trade in base units
    EXPECT_EQ((*one)[0].trade_id, 1001258392876510000ULL);
    EXPECT_EQ((*one)[0].ts_ms, 1786431073882ULL);
    EXPECT_TRUE((*one)[0].aggressor_bought);

    const auto batch = load("ws_trades_batch.json");
    const auto many = parse_trades(batch, storage);
    ASSERT_TRUE(many.has_value());
    ASSERT_EQ(many->size(), 4U);
    EXPECT_EQ((*many)[0].contract_code, "ETH-USDT");
    EXPECT_EQ((*many)[3].amount, "890");
    EXPECT_FALSE((*many)[3].aggressor_bought);
    // Consecutive ids off one match, which is why the tail must not be dropped.
    EXPECT_EQ((*many)[3].trade_id - (*many)[0].trade_id, 3U);
}

TEST(HtxWire, ParsesDepthSnapshot) {
    LevelBuffer storage;
    const auto frame = load("ws_depth_step0.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract_code, "BTC-USDT");
    EXPECT_EQ(wire->ts_ms, 1786431109503ULL);
    ASSERT_EQ(wire->bids.size(), 5U);
    ASSERT_EQ(wire->asks.size(), 5U);
    EXPECT_EQ(wire->bids[0].price, "63920.1");
    EXPECT_EQ(wire->bids[4].price, "63919.7"); // descending
    EXPECT_EQ(wire->asks[0].price, "63920.2");
    EXPECT_EQ(wire->asks[4].price, "63922.5"); // ascending
    EXPECT_EQ(wire->asks[0].qty, "4485");
}

TEST(HtxWire, Step0IsAWholeBookAndNotAnIncrement) {
    // The evidence for treating this venue as a snapshot venue: the live frame
    // carries every level, not the ones that changed, and nothing on it names
    // an action or a previous sequence.
    LevelBuffer storage;
    const auto frame = load("ws_depth_step0_full.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->bids.size(), 150U);
    EXPECT_EQ(wire->asks.size(), 150U);
    // `version` is epoch SECONDS and repeats across the ten frames HTX sends
    // in one; mrid is what actually orders them.
    EXPECT_EQ(wire->version, 1786982545ULL);
    EXPECT_EQ(wire->mrid, 100126224898536ULL);
}

TEST(HtxWire, RefusesABookDeeperThanTheBuffer) {
    // step0 ships about 150 levels a side today; the ceiling exists so a
    // hostile frame cannot make the gateway allocate.
    std::string frame = R"({"ch":"market.BTC-USDT.depth.step0","ts":1,"tick":{"bids":[)";
    for (std::size_t i = 0; i <= k_max_levels; ++i) {
        if (i != 0) {
            frame += ',';
        }
        frame += "[1,1]";
    }
    frame += R"(],"asks":[],"ts":1,"version":1}})";

    LevelBuffer storage;
    const auto wire = parse_depth(frame, storage);
    ASSERT_FALSE(wire.has_value());
    EXPECT_EQ(wire.error(), WireError::TooManyEntries);
}

TEST(HtxWire, ParsesTheFundingNotification) {
    const auto frame = load("ws_notify_funding.json");
    const auto wire = parse_funding(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract_code, "BTC-USDT");
    EXPECT_EQ(wire->funding_rate, "0.000100000000000000");
    EXPECT_TRUE(wire->estimated_rate.empty()); // JSON null, not a rate of zero
    // settlement_time is the next settlement; funding_time is when HTX built
    // the message and moves every few seconds. GET /swap_funding_rate gives
    // `funding_time` the OTHER meaning, which is why only this shape is read.
    EXPECT_EQ(wire->settlement_ms, 1786435200000ULL);
    EXPECT_EQ(wire->funding_ms, 1786431110000ULL);
    EXPECT_EQ(wire->ts_ms, 1786431119069ULL);
}

TEST(HtxWire, DetailCarriesTheTwoTurnoverColumnsAndOneTrap) {
    const auto frame = load("ws_detail.json");
    const auto wire = parse_detail(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract_code, "BTC-USDT");
    // amount is BASE, trade_turnover is QUOTE: their quotient, 80670.9, sits
    // between this frame's low (78931.3) and high (82222.2). The third figure
    // on the tick, `vol` 8021738, is the same window in CONTRACTS — exactly
    // amount / 0.001, the contract size — and is not surfaced at all.
    EXPECT_EQ(wire->amount, "8021.738");
    EXPECT_EQ(wire->trade_turnover, "647118134.3374");
    EXPECT_EQ(wire->ts_ms, 1788532163204ULL);
}

TEST(HtxWire, RefusesAFrameOfTheWrongShape) {
    // A parser aimed at the wrong topic must fail, not fill a struct with
    // plausible zeros.
    EXPECT_FALSE(parse_bbo(load("ws_trade.json")).has_value());
    EXPECT_FALSE(parse_funding(load("ws_bbo.json")).has_value());
    EXPECT_FALSE(parse_event(R"({"ch":)").has_value());
    EXPECT_FALSE(parse_event("").has_value());
    EXPECT_FALSE(parse_stream_error(load("ws_subscribe_ack.json")).has_value());

    TradeBuffer trades;
    EXPECT_FALSE(parse_trades(load("ws_bbo.json"), trades).has_value());
    LevelBuffer levels;
    EXPECT_FALSE(parse_depth(load("ws_bbo.json"), levels).has_value());
}

// ---------------------------------------------------------- rest wire ----

TEST(HtxWire, ReadsBothRestEnvelopeSpellings) {
    const auto ok_body = load("rest_contracts.json");
    const auto ok = parse_rest_envelope(ok_body);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->ok);

    // The linear-swap-api family: a NUMERIC err_code.
    const auto numeric_body = load("rest_error_bad_contract.json");
    const auto numeric = parse_rest_envelope(numeric_body);
    ASSERT_TRUE(numeric.has_value());
    EXPECT_FALSE(numeric->ok);
    EXPECT_EQ(numeric->code, "1014");
    EXPECT_EQ(numeric->msg, "This contract doesnt exist.");

    // The linear-swap-ex family: a hyphenated WORD, under a hyphenated key.
    const auto word_body = load("rest_error_invalid_parameter.json");
    const auto word = parse_rest_envelope(word_body);
    ASSERT_TRUE(word.has_value());
    EXPECT_FALSE(word->ok);
    EXPECT_EQ(word->code, k_invalid_parameter);
    EXPECT_EQ(word->msg, "invalid type:stepxx");
}

TEST(HtxWire, RefusesABodyThatIsNotAnEnvelope) {
    // HTX answers an unrouted path with a Spring error whose `status` is the
    // NUMBER 404. Reading that as a venue verdict would hide a routing bug.
    EXPECT_FALSE(parse_rest_envelope(load("rest_error_not_found.json")).has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"ts":1})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json").has_value());
    // "error" with nothing naming the error is not something to act on.
    EXPECT_FALSE(parse_rest_envelope(R"({"status":"error","ts":1})").has_value());
}

TEST(HtxWire, ParsesContracts) {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);

    EXPECT_EQ((*rows)[0].contract_code, "BTC-USDT");
    EXPECT_EQ((*rows)[0].symbol, "BTC");
    EXPECT_EQ((*rows)[0].contract_status, 1);
    EXPECT_EQ((*rows)[0].business_type, "swap");
    EXPECT_EQ((*rows)[0].settlement_period_h, 8);
    EXPECT_EQ((*rows)[0].settlement_ms, 1786435200000ULL);
    // Carried as the token HTX wrote, trailing zeros and all.
    EXPECT_EQ((*rows)[0].price_tick, "0.100000000000000000");
    EXPECT_EQ((*rows)[0].contract_size, "0.001000000000000000");

    // Scientific notation, straight from the venue on two live rows.
    EXPECT_EQ((*rows)[2].contract_code, "SHIB-USDT");
    EXPECT_EQ((*rows)[2].price_tick, "1.000000000E-9");
    EXPECT_EQ((*rows)[3].contract_code, "PEPE-USDT");
    EXPECT_EQ((*rows)[3].price_tick, "1.00000000E-10");

    // A four-hour funding interval and a status that is not 1.
    EXPECT_EQ((*rows)[4].contract_code, "CYBER-USDT");
    EXPECT_EQ((*rows)[4].contract_status, 3);
    EXPECT_EQ((*rows)[4].settlement_period_h, 4);
    EXPECT_EQ((*rows)[4].delivery_time, "1767859200000");
}

TEST(HtxWire, ParsesServerTime) {
    const auto time = parse_server_time(load("rest_server_time.json"));
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(*time, 1786430907715ULL);
    EXPECT_FALSE(parse_server_time(load("rest_error_not_found.json")).has_value());
    EXPECT_FALSE(parse_server_time(load("rest_error_bad_contract.json")).has_value());
}

// ---------------------------------------------------------------- map ----

TEST(HtxMap, ReadsBothNumberSpellingsExactly) {
    EXPECT_EQ(exact_e9("0.1")->raw, 100'000'000);
    EXPECT_EQ(exact_e9("0.100000000000000000")->raw, 100'000'000);
    EXPECT_EQ(exact_e9("63920.1")->raw, 63'920'100'000'000);
    EXPECT_EQ(exact_e9("1455")->raw, 1455 * k_e9);
    EXPECT_EQ(exact_e9("-0.5")->raw, -500'000'000);
    // The spellings parse_decimal_e9 refuses outright, both live on the
    // catalogue: 1e-9 is representable, 1e-10 is not.
    EXPECT_EQ(exact_e9("1.000000000E-9")->raw, 1);
    EXPECT_FALSE(exact_e9("1.00000000E-10").has_value());
    EXPECT_EQ(exact_e9("1000000.000000000000000000")->raw, 1'000'000 * k_e9);

    EXPECT_FALSE(exact_e9("").has_value());
    EXPECT_FALSE(exact_e9("1.2.3").has_value());
    EXPECT_FALSE(exact_e9("abc").has_value());
    EXPECT_FALSE(exact_e9("1e").has_value());

    // Rates truncate where prices refuse: HTX pads every funding rate to
    // eighteen digits and the tail is not always zero.
    EXPECT_EQ(rate_e9("0.000100000000000000")->raw, 100'000);
    EXPECT_EQ(rate_e9("-0.000058784590419109")->raw, -58'784);
    EXPECT_FALSE(exact_e9("-0.000058784590419109").has_value());
    EXPECT_FALSE(rate_e9("").has_value());
}

TEST(HtxMap, BuildsInstrumentsAndRefusesTheOnesItCannotTrade) {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());

    const auto btc = map_instrument((*rows)[0], MarketType::Perp);
    ASSERT_TRUE(btc.has_value());
    const auto& base = base_of(*btc);
    EXPECT_EQ(base.exchange_symbol.view(), "BTC-USDT");
    EXPECT_EQ(base.tick_size.raw, 100'000'000);    // 0.1
    EXPECT_EQ(base.lot_size.raw, k_e9);            // one contract, always
    EXPECT_EQ(base.contract_value.raw, 1'000'000); // 0.001 BTC per contract
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*btc));
    // Stated by the catalogue, where okx has to infer it from funding frames.
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 8U * 3600U);

    const auto shib = map_instrument((*rows)[2], MarketType::Perp);
    ASSERT_TRUE(shib.has_value());
    EXPECT_EQ(base_of(*shib).tick_size.raw, 1); // the finest FixedE9 holds
    EXPECT_EQ(base_of(*shib).contract_value.raw, 1000 * k_e9);

    // A 1e-10 tick cannot be represented without rounding it to zero, and a
    // zero step divides every later price conversion.
    EXPECT_FALSE(map_instrument((*rows)[3], MarketType::Perp).has_value());
    // status 3: still shipping usable numbers, so only the status keeps it out.
    EXPECT_FALSE(map_instrument((*rows)[4], MarketType::Perp).has_value());
    // This endpoint is the USDT-M swap catalogue and nothing else.
    EXPECT_FALSE(map_instrument((*rows)[0], MarketType::Spot).has_value());
}

TEST(HtxMap, MapsBbo) {
    const auto table = catalogue();
    const auto frame = load("ws_bbo.json");
    const auto wire = parse_bbo(frame);
    ASSERT_TRUE(wire.has_value());
    const auto bbo = map_bbo(*wire, table, UnixNanos{99});
    ASSERT_TRUE(bbo.has_value());
    const auto& base = base_for(table, "BTC-USDT");
    EXPECT_EQ(bbo->instrument.packed, base.id.packed);
    EXPECT_EQ(bbo->bid.raw, 639'201); // 63920.1 / 0.1
    EXPECT_EQ(bbo->ask.raw, 639'202);
    EXPECT_EQ(bbo->bid_qty.raw, 1455); // already a count of contracts
    EXPECT_EQ(bbo->ask_qty.raw, 4485);
    EXPECT_EQ(bbo->ts_event.ns, 1786431108849ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, 99U);
}

TEST(HtxMap, DropsAQuoteThatDoesNotDivideByTheStep) {
    const auto table = catalogue();
    WireBbo wire{};
    wire.contract_code = "BTC-USDT";
    wire.bid_price = "63920.15"; // finer than the 0.1 tick HTX states
    wire.bid_qty = "1455";
    wire.ask_price = "63920.2";
    wire.ask_qty = "4485";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    wire.bid_price = "63920.1";
    wire.bid_qty = "1455.5"; // half a contract is not a size HTX can mean
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());

    // An instrument the catalogue refused has no step to divide by.
    wire.bid_qty = "1455";
    wire.contract_code = "PEPE-USDT";
    EXPECT_FALSE(map_bbo(wire, table, UnixNanos{1}).has_value());
}

TEST(HtxMap, TakesTheAggressorFromTheDirection) {
    const auto table = catalogue();
    TradeBuffer storage;
    const auto frame = load("ws_trades_batch.json");
    const auto wire = parse_trades(frame, storage);
    ASSERT_TRUE(wire.has_value());
    ASSERT_EQ(wire->size(), 4U);

    // "sell": HTX names the TAKER's side, so nothing is inverted here.
    const auto sold = map_trade((*wire)[0], table, UnixNanos{7});
    ASSERT_TRUE(sold.has_value());
    EXPECT_EQ(sold->aggressor, Side::Sell);
    EXPECT_EQ(sold->price.raw, 187'259); // 1872.59 / 0.01
    EXPECT_EQ(sold->qty.raw, 20);        // contracts, from `amount`
    EXPECT_EQ(sold->ts_event.ns, 1786431135430ULL * 1'000'000ULL);

    WireTrade taker = (*wire)[0];
    taker.aggressor_bought = true;
    const auto bought = map_trade(taker, table, UnixNanos{7});
    ASSERT_TRUE(bought.has_value());
    EXPECT_EQ(bought->aggressor, Side::Buy);
}

TEST(HtxMap, MapsFundingFromTheNotification) {
    const auto table = catalogue();
    const auto frame = load("ws_notify_funding.json");
    const auto wire = parse_funding(frame);
    ASSERT_TRUE(wire.has_value());
    const auto funding = map_funding(*wire, table, UnixNanos{1234});
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 100'000); // 0.0001
    // settlement_time, not funding_time: the other reading puts the next
    // settlement a few seconds from now, forever.
    EXPECT_EQ(funding->next_funding_time.ns, 1786435200000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->interval_s, 8U * 3600U);
    // estimated_rate is null; echoing the current rate would fabricate one.
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    // HTX ships no mark on this frame, and a zero would read as a real price.
    EXPECT_TRUE(funding->mark_price.is_undef());
    EXPECT_EQ(funding->ts_event.ns, 1786431110000ULL * 1'000'000ULL);
    EXPECT_EQ(funding->ts_init.ns, 1234U);

    WireFunding silent = *wire;
    silent.funding_rate = {};
    // No rate at all is not the same as a rate of zero.
    EXPECT_FALSE(map_funding(silent, table, UnixNanos{1}).has_value());
}

TEST(HtxMap, MapsTurnoverFromTheDetailTick) {
    const auto table = catalogue();
    const auto frame = load("ws_detail.json");
    const auto wire = parse_detail(frame);
    ASSERT_TRUE(wire.has_value());
    const auto stats = map_ticker_stats(*wire, table, UnixNanos{4321});
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->volume_24h_base.raw, 8'021'738'000'000LL);    // 8021.738 BTC
    EXPECT_EQ(stats->volume_24h_quote.raw, 647'118'134'337'400LL); // 647118134.3374 USDT
    EXPECT_EQ(stats->ts_event.ns, 1788532163204ULL * 1'000'000ULL);
    EXPECT_EQ(stats->ts_init.ns, 4321U);

    // A tick that states neither column says nothing about turnover, and a
    // contract the catalogue never loaded has no cell to write to. Publishing
    // a zero for either would read as a market that stopped trading.
    WireDetail silent = *wire;
    silent.amount = {};
    silent.trade_turnover = {};
    EXPECT_FALSE(map_ticker_stats(silent, table, UnixNanos{1}).has_value());
    WireDetail unknown = *wire;
    unknown.contract_code = "NOSUCH-USDT";
    EXPECT_FALSE(map_ticker_stats(unknown, table, UnixNanos{1}).has_value());
}

// ----------------------------------------------------------------- ws ----

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

TEST(HtxMap, MapsDepthToABookSharedMemoryCanTake) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = load("ws_depth_step0.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_depth(*wire, table, UnixNanos{99});
    ASSERT_TRUE(book.has_value());
    expect_coherent(*book);
    EXPECT_EQ(book->instrument.packed, base_for(table, "BTC-USDT").id.packed);
    EXPECT_EQ(book->bid_count, 5U);
    EXPECT_EQ(book->ask_count, 5U);
    EXPECT_EQ(book->bids[0].price.raw, 639'201); // 63920.1 over the 0.1 tick
    EXPECT_EQ(book->asks[0].price.raw, 639'202);
    EXPECT_EQ(book->asks[0].qty.raw, 4485); // contracts, as bbo quotes them
    EXPECT_EQ(book->ts_event.ns, 1786431109503ULL * 1'000'000ULL);
    EXPECT_EQ(book->ts_init.ns, 99U);
    EXPECT_EQ(book->venue_seq, 100125839300207ULL) << "mrid is the forensic handle";
}

TEST(HtxMap, KeepsTheTopHundredOfADeeperBook) {
    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = load("ws_depth_step0_full.json");
    const auto wire = parse_depth(frame, storage);
    ASSERT_TRUE(wire.has_value());

    const auto book = map_depth(*wire, table, UnixNanos{1});
    ASSERT_TRUE(book.has_value());
    expect_coherent(*book);
    EXPECT_EQ(book->bid_count, k_book_levels);
    EXPECT_EQ(book->ask_count, k_book_levels);
    // The hundredth level off the wire, so the cut is a cut and not a resort.
    EXPECT_EQ(book->bids[99].price.raw, 641'162); // 64116.2
    EXPECT_EQ(book->asks[99].price.raw, 641'793); // 64179.3
}

TEST(HtxMap, ALevelOffTheStepTruncatesTheBookRatherThanRoundingIt) {
    // Safe here because the next frame is another whole book: a truncated
    // snapshot heals in 100 ms, where a delta venue would diverge forever.
    const auto table = catalogue();
    const std::array<WireLevel, 3> bids{WireLevel{.price = "63920.1", .qty = "10"},
                                        WireLevel{.price = "63920.05", .qty = "10"},
                                        WireLevel{.price = "63919.9", .qty = "10"}};
    const std::array<WireLevel, 2> asks{WireLevel{.price = "63920.2", .qty = "4"},
                                        WireLevel{.price = "63920.3", .qty = "0.5"}};
    WireDepth wire{};
    wire.contract_code = "BTC-USDT";
    wire.bids = bids;
    wire.asks = asks;

    const auto book = map_depth(wire, table, UnixNanos{1});
    ASSERT_TRUE(book.has_value());
    expect_coherent(*book);
    EXPECT_EQ(book->bid_count, 1U) << "0.05 past a 0.1 tick stops the side";
    EXPECT_EQ(book->ask_count, 1U) << "half a contract is not a size HTX means";
}

TEST(HtxMap, ASizeOfZeroIsNoLevelAndAnEmptySidePublishesNothing) {
    const auto table = catalogue();
    const std::array<WireLevel, 2> bids{WireLevel{.price = "63920.1", .qty = "0"},
                                        WireLevel{.price = "63920", .qty = "7"}};
    const std::array<WireLevel, 1> asks{WireLevel{.price = "63920.2", .qty = "4"}};
    WireDepth wire{};
    wire.contract_code = "BTC-USDT";
    wire.bids = bids;
    wire.asks = asks;

    const auto book = map_depth(wire, table, UnixNanos{1});
    ASSERT_TRUE(book.has_value());
    ASSERT_EQ(book->bid_count, 1U);
    EXPECT_EQ(book->bids[0].price.raw, 639'200) << "the zero is gone, not resting at zero";

    // A book with nothing on one side is not a book, and half of one crossing
    // into shm reads to a consumer as a market with no offer.
    wire.asks = {};
    EXPECT_FALSE(map_depth(wire, table, UnixNanos{1}).has_value());
}

TEST(HtxWs, BuildsSubscriptionFrames) {
    Protocol protocol;
    const auto first = protocol.subscribe_frame("BTC-USDT", topic_suffix(exec::SubKind::Bbo));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text, R"({"sub":"market.BTC-USDT.bbo","id":"1"})");

    // The id is echoed in the answer, so it has to move.
    const auto second = protocol.subscribe_frame("ETH-USDT", topic_suffix(exec::SubKind::Trades));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text, R"({"sub":"market.ETH-USDT.trade.detail","id":"2"})");

    const auto depth = protocol.subscribe_frame("BTC-USDT", topic_suffix(exec::SubKind::Depth));
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->text, R"({"sub":"market.BTC-USDT.depth.step0","id":"3"})");

    const auto off = protocol.unsubscribe_frame("BTC-USDT", "bbo");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"unsub":"market.BTC-USDT.bbo","id":"4"})");

    // Funding is the other socket, and its dialect names the op instead.
    const auto funding = protocol.funding_subscribe_frame("BTC-USDT");
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->text, R"({"op":"sub","topic":"public.BTC-USDT.funding_rate","cid":"5"})");
}

TEST(HtxWs, AnswersEachKeepaliveWithTheTokenItWasGiven) {
    // The end-to-end claim: a real ping in, the exact pong out. HTX drops a
    // connection whose pong does not match the value byte for byte.
    Protocol protocol;
    const auto market = load("ws_ping.json");
    const auto market_event = parse_event(market);
    ASSERT_TRUE(market_event.has_value());
    const auto market_pong = protocol.market_pong_frame(market_event->keepalive_ts);
    ASSERT_TRUE(market_pong.has_value());
    EXPECT_EQ(market_pong->text, R"({"pong":1786431113951})");

    const auto notify = load("ws_notify_ping.json");
    const auto notify_event = parse_event(notify);
    ASSERT_TRUE(notify_event.has_value());
    const auto notify_pong = protocol.notify_pong_frame(notify_event->keepalive_ts);
    ASSERT_TRUE(notify_pong.has_value());
    // The quotes come from HTX, not from us: it sends the same instant as a
    // string here and a number there.
    EXPECT_EQ(notify_pong->text, R"({"op":"pong","ts":"1786431124111"})");

    EXPECT_FALSE(protocol.market_pong_frame({}).has_value());
    EXPECT_FALSE(protocol.notify_pong_frame({}).has_value());
}

TEST(HtxWs, RefusesToBuildAFrameItCannotName) {
    Protocol protocol;
    // Funding lives on the other socket and mark price on a third; sending ""
    // would subscribe to a topic HTX refuses with bad-request.
    EXPECT_TRUE(topic_suffix(exec::SubKind::Funding).empty());
    EXPECT_TRUE(topic_suffix(exec::SubKind::MarkPrice).empty());
    EXPECT_TRUE(topic_suffix(exec::SubKind::Kline).empty());
    EXPECT_EQ(protocol.subscribe_frame("BTC-USDT", {}).error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.subscribe_frame({}, "bbo").error(), WsBuildError::UnsupportedKind);
    EXPECT_EQ(protocol.funding_subscribe_frame({}).error(), WsBuildError::UnsupportedKind);

    const std::string huge(400, 'X');
    EXPECT_EQ(protocol.subscribe_frame(huge, "bbo").error(), WsBuildError::BufferTooSmall);
    EXPECT_EQ(protocol.funding_subscribe_frame(huge).error(), WsBuildError::BufferTooSmall);
    EXPECT_EQ(protocol.market_pong_frame(huge).error(), WsBuildError::BufferTooSmall);
}

TEST(HtxWs, NamesTheTwoEndpointsOnOneHost) {
    EXPECT_EQ(stream_host(), "api.hbdm.com");
    EXPECT_EQ(stream_port(), "443");
    EXPECT_EQ(market_path(), "/linear-swap-ws");
    EXPECT_EQ(notify_path(), "/linear-swap-notification");
    EXPECT_EQ(rest_host(), "api.hbdm.com");
}

TEST(HtxRest, BuildsUnauthenticatedReads) {
    EXPECT_EQ(RequestBuilder::contracts().target, "/linear-swap-api/v1/swap_contract_info");
    EXPECT_EQ(RequestBuilder::contracts().method, net::HttpMethod::Get);
    // Outside both market-data families, hence the bare path.
    EXPECT_EQ(RequestBuilder::server_time().target, "/api/v1/timestamp");
}

// ------------------------------------------------------------- ledger ----

TEST(HtxWire, FinancialRecordsSitInAFlatDataArrayWithNoPageWrapper) {
    static thread_local FinancialRecordBuffer storage;
    const auto body = load("rest_swap_financial_record.json");
    const auto rows = parse_financial_records(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U) << "v3 dropped the data.financial_record wrapper v1 had";
    EXPECT_EQ((*rows)[0].contract_code, "BTC-USDT");
    EXPECT_EQ((*rows)[0].type, 31);
    EXPECT_EQ((*rows)[0].query_id, 138'798'248ULL);
    EXPECT_EQ((*rows)[0].ts_ms, 1'638'758'435'635ULL);
    // The raw token, padded the way HTX's own serialiser pads it: a typed read
    // through a double would not come back the same number.
    EXPECT_EQ((*rows)[0].amount, "-0.024464850000000000");
    EXPECT_EQ((*rows)[0].asset, "USDT");
}

TEST(HtxMap, AFundingPaymentAndAFundingReceiptDoNotBothComeOutPositive) {
    // THE BUG THIS EXISTS FOR: `amount` is the signed balance move, and a
    // mapper that took its magnitude would report every payment as income.
    const auto table = catalogue();
    static thread_local FinancialRecordBuffer storage;
    const auto body = load("rest_swap_financial_record.json");
    const auto rows = parse_financial_records(body, storage);
    ASSERT_TRUE(rows.has_value());

    std::vector<LedgerEntry> booked;
    for (const auto& wire : *rows) {
        if (const auto charge = map_financial_record(wire, table, UnixNanos{7});
            charge.has_value()) {
            booked.push_back(*charge);
        }
    }
    // Type 5 is an opening fee the fill already booked; the contradictory row
    // and the contract this catalogue does not carry are dropped below.
    ASSERT_EQ(booked.size(), 2U);

    EXPECT_EQ(booked[0].kind, LedgerKind::Funding);
    EXPECT_EQ(booked[0].amount.raw, -24'464'850) << "type 31, expenditure";
    EXPECT_EQ(booked[1].amount.raw, 13'820'000) << "type 30, income, and NOT flipped";
    EXPECT_EQ(booked[0].settled_at.ns, 1'638'758'435'635ULL * 1'000'000ULL);
    // query_id, not `id`: HTX documents `id` as unique per CONTRACT, so two
    // contracts can carry the same one.
    EXPECT_EQ(booked[0].reference.view(), "138798248");
    EXPECT_EQ(booked[0].ts_init.ns, 7U);
}

TEST(HtxMap, ARowWhoseTypeAndAmountDisagreeAboutDirectionIsDropped) {
    // HTX states the direction TWICE — once as 30/31, once as the amount's
    // sign — and states neither convention in prose. Resolving a disagreement
    // by preferring one puts a number in an append-only ledger that the
    // venue's own books contradict, so the row is refused instead.
    const auto table = catalogue();
    static thread_local FinancialRecordBuffer storage;
    const auto body = load("rest_swap_financial_record.json");
    const auto rows = parse_financial_records(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ((*rows)[3].type, 31);
    ASSERT_EQ((*rows)[3].amount, "0.009900000000000000") << "an expenditure that credited";
    EXPECT_FALSE(map_financial_record((*rows)[3], table, UnixNanos{7}).has_value());

    // And a contract this gateway never loaded maps to nothing rather than to
    // a neighbour.
    ASSERT_EQ((*rows)[4].contract_code, "DOGE-USDT");
    EXPECT_FALSE(map_financial_record((*rows)[4], table, UnixNanos{7}).has_value());
}

TEST(HtxRest, TheLedgerReadNamesTheMarginAccountAndClampsToTheVenuesWindow) {
    net::Signer signer;
    signer.set_api_key("htx-test-key");
    ASSERT_TRUE(signer.secret().assign("htx-test-secret").has_value());
    SignedRequestBuilder builder{signer};

    // 48 hours before this instant, to the millisecond.
    constexpr UnixNanos now{1'700'000'000'000'000'000ULL};
    constexpr std::uint64_t floor_ms = 1'700'000'000'000ULL - (48ULL * 60 * 60 * 1000);

    const auto inside = UnixNanos{1'699'900'000'000ULL * 1'000'000ULL};
    const auto request = builder.financial_record(MarketType::Perp, inside, now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_TRUE(std::string_view{request->target}.starts_with(
        "/linear-swap-api/v3/swap_financial_record?"));
    // No contract_code: the whole margin account in one call, which is what
    // makes this pull answerable where the per-contract trade history is not.
    EXPECT_EQ(request->body, R"({"mar_acct":"USDT","type":"30,31","start_time":1699900000000})");

    // A cursor older than the window HTX answers is CLAMPED, not sent: a wider
    // start is refused outright, and a refused call reports no funding at all.
    const auto clamped = builder.financial_record(MarketType::Perp, UnixNanos{0}, now);
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->body,
              R"({"mar_acct":"USDT","type":"30,31","start_time":)" + std::to_string(floor_ms) +
                  "}");

    EXPECT_EQ(builder.financial_record(MarketType::Spot, inside, now).error(),
              BuildError::SpotHasNoFunding);
}

// ------------------------------------------------------------- errors ----

TEST(HtxErrors, ClassifiesWhatTheVenueActuallySaid) {
    // One classifier, three spellings: a number from REST, a number from the
    // notification socket, a word from the market socket.
    EXPECT_EQ(classify("403"), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify("1014"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("1332"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("2011"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_ws_bad_request), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_parameter), exec::ErrorClass::Terminal);
    // Unknown codes are Terminal, never Retryable: retrying an unrecognised
    // refusal at full speed is how an IP earns a ban.
    EXPECT_EQ(classify("999999"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify("something-new"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(""), exec::ErrorClass::Terminal);
}

TEST(HtxErrors, ClassifiesTheHttpStatusWhenThereIsNoEnvelope) {
    EXPECT_EQ(classify_http(429), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify_http(503), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_http(500), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_http(0), exec::ErrorClass::Connectivity);
    EXPECT_EQ(classify_http(200), exec::ErrorClass::Terminal);
    // The Spring 404 an unrouted path answers with.
    EXPECT_EQ(classify_http(404), exec::ErrorClass::Terminal);
}

TEST(HtxWire, ReadsFieldsInAnyOrderAndSkipsWhatItDoesNotKnow) {
    const std::string depth =
        R"({"tick":{"version":7,"asks":[[2.5,3]],"extra":{"x":[1,2]},"bids":[[2,4],[1.5,1]],)"
        R"("ts":9},"ch":"market.ETH-USDT.depth.step0"})";
    const auto event = parse_event(depth);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Depth);
    EXPECT_EQ(event->contract_code, "ETH-USDT");
    LevelBuffer levels;
    const auto wire = parse_depth(depth, levels);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->mrid, 0U) << "absent reads as zero, as before";
    EXPECT_EQ(wire->version, 7U);
    EXPECT_EQ(wire->ts_ms, 9U);
    ASSERT_EQ(wire->bids.size(), 2U);
    EXPECT_EQ(wire->bids[1].price, "1.5");
    ASSERT_EQ(wire->asks.size(), 1U);
    EXPECT_EQ(wire->asks[0].qty, "3");

    TradeBuffer trades;
    const auto trade = parse_trades(
        R"({"ch":"market.ETH-USDT.trade.detail","tick":{"data":[{"direction":"sell","price":2.5,)"
        R"("id":4,"amount":2}]}})",
        trades);
    ASSERT_TRUE(trade.has_value());
    ASSERT_EQ(trade->size(), 1U);
    EXPECT_EQ((*trade)[0].price, "2.5");
    EXPECT_EQ((*trade)[0].quantity, "");
    EXPECT_EQ((*trade)[0].ts_ms, 0U);
    EXPECT_FALSE((*trade)[0].aggressor_bought);
    EXPECT_FALSE(parse_trades(R"({"ch":"market.ETH-USDT.trade.detail","tick":{"data":[)"
                              R"({"price":2.5,"id":4,"amount":2}]}})",
                              trades)
                     .has_value())
        << "direction is required";
}

AssetClass htx_class(std::string_view code, std::string_view labels) {
    WireContract wire{};
    wire.contract_code = code;
    wire.symbol = code.substr(0, code.find('-'));
    wire.contract_size = "0.01";
    wire.price_tick = "0.01";
    wire.business_type = "swap";
    wire.contract_type = "swap";
    wire.contract_status = 1;
    wire.settlement_period_h = 8;
    wire.tradfi_labels = labels;
    const auto mapped = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(mapped.has_value());
    return mapped.has_value() ? base_of(*mapped).asset_class : AssetClass::Unknown;
}

TEST(HtxMap, TradfiLabelsClassifyTheUnderlying) {
    EXPECT_EQ(htx_class("NVDA-USDT", R"(["Stocks"])"), AssetClass::Equity);
    EXPECT_EQ(htx_class("SPY-USDT", R"(["Indices"])"), AssetClass::Equity);
    EXPECT_EQ(htx_class("XAU-USDT", R"(["Metals"])"), AssetClass::Commodity);
    EXPECT_EQ(htx_class("BTC-USDT", "[]"), AssetClass::Crypto);
    EXPECT_EQ(htx_class("TBT-USDT", R"(["Bonds"])"), AssetClass::Unknown);
}

} // namespace
} // namespace raikiri::venues::htx
