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

// bingx's deepest depth tier
constexpr std::uint32_t k_bingx_depth = 100;

std::string load_bytes(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "missing fixture " << path.string();
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// fixtures end in a newline; live frames do not
std::string load(std::string_view name) {
    auto text = load_bytes(name);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

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
    const auto adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Bingx);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(BingxVenue, AnOrderWithNoCredentialsIsRefusedLoudly) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
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
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
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
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto orders = adapter->execution->open_order_reports(MarketType::Perp);
    ASSERT_FALSE(orders.has_value());
    EXPECT_EQ(orders.error().cls, exec::ErrorClass::AuthFailure);
    const auto positions = adapter->execution->position_reports(MarketType::Perp);
    ASSERT_FALSE(positions.has_value());
    EXPECT_EQ(positions.error().cls, exec::ErrorClass::AuthFailure);

    const auto spot_positions = adapter->execution->position_reports(MarketType::Spot);
    ASSERT_FALSE(spot_positions.has_value());
    EXPECT_EQ(spot_positions.error().message.view(), "bingx spot position pull");
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Perp, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->fill_reports(MarketType::Spot, UnixNanos{0}).has_value());
    EXPECT_FALSE(adapter->execution->connected());
}

TEST(BingxVenue, EachSocketRefusesTheOtherMarketsInstruments) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    const auto spot = adapter->data->subscribe(exec::SubKind::Bbo,
                                               InstrumentId::make(VenueId::Bingx, MarketType::Spot, SymbolId{0}));
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().cls, exec::ErrorClass::Terminal);

    auto spot_adapter = exec::make_venue({.venue = VenueId::Bingx, .market = MarketType::Spot}, sink, clock);
    ASSERT_TRUE(spot_adapter.has_value());
    const auto perp = spot_adapter->data->subscribe(exec::SubKind::Bbo,
                                                    InstrumentId::make(VenueId::Bingx, MarketType::Perp, SymbolId{0}));
    ASSERT_FALSE(perp.has_value());
    EXPECT_EQ(perp.error().cls, exec::ErrorClass::Terminal);
}

TEST(BingxInflate, RestoresACompressedFrame) {
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
    // reset() between frames or the second decodes against the first's window
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

TEST(BingxWire, ClassifiesEveryChannelAndItsSymbol) {
    struct Row {
        std::string_view fixture;
        EventKind kind;
    };

    for (const auto& row : {Row{"ws_book_ticker.json", EventKind::BookTicker},
                            Row{"ws_trade.json", EventKind::Trade},
                            Row{"ws_depth100.json", EventKind::Depth},
                            Row{"ws_mark_price.json", EventKind::MarkPrice},
                            Row{"ws_ticker.json", EventKind::Ticker}}) {
        const auto frame = load(row.fixture);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << row.fixture;
        EXPECT_EQ(event->kind, row.kind) << row.fixture;
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
    // named strings: the parsers return views into them
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
    EXPECT_EQ(wire->ts_ms, 1786983056064ULL); // beside data
    ASSERT_EQ(wire->bids.size(), k_bingx_depth);
    ASSERT_EQ(wire->asks.size(), k_bingx_depth);
    EXPECT_EQ(wire->bids[0].price, "64071.2");
    EXPECT_EQ(wire->bids[k_bingx_depth - 1].price, "64005.6"); // descending
    EXPECT_EQ(wire->asks[0].price, "64071.3");
    EXPECT_EQ(wire->asks[k_bingx_depth - 1].price, "64162.1"); // ascending
    EXPECT_EQ(wire->asks[0].qty, "78.6122");
}

TEST(BingxWire, RefusesAFrameOfTheWrongShape) {
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
    // v is base and q quote
    EXPECT_EQ(wire->volume, "21355.7400");
    EXPECT_EQ(wire->quote_volume, "1723468750.41");
    EXPECT_EQ(wire->event_ms, 1788532398984ULL);

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
    EXPECT_EQ(stats->volume_24h_base.raw, 21'355'740'000'000LL);     // 21355.7400 BTC
    EXPECT_EQ(stats->volume_24h_quote.raw, 1'723'468'750'410'000LL); // 1723468750.41 USDT
    EXPECT_EQ(stats->ts_event.ns, 1788532398984ULL * 1'000'000ULL);
    EXPECT_EQ(stats->ts_init.ns, 99U);

    EXPECT_FALSE(map_ticker_stats(*wire, table, MarketType::Spot, UnixNanos{99}).has_value());

    WireTicker silent = *wire;
    silent.volume = {};
    silent.quote_volume = {};
    EXPECT_FALSE(map_ticker_stats(silent, table, MarketType::Perp, UnixNanos{1}).has_value());
}

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

    const auto missing_body = load("rest_error_not_found.json");
    const auto missing = parse_rest_envelope(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, k_endpoint_not_found);
}

TEST(BingxWire, AnOnlineContractCanStillBeClosedToTheApi) {
    // apiStateOpen false refuses orders on online perps
    // the body is named because parsed rows view into it
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);

    EXPECT_TRUE((*rows)[0].api_open);
    EXPECT_TRUE((*rows)[0].api_close);
    EXPECT_EQ((*rows)[4].symbol, "POWER-USDT");
    EXPECT_EQ((*rows)[4].status, 1);
    EXPECT_FALSE((*rows)[4].api_open);
    EXPECT_FALSE((*rows)[4].api_close);

    const auto table = catalogue();
    EXPECT_NE(table.find("POWER-USDT"), nullptr);
}

TEST(BingxWire, ARowStatingNeitherApiStateIsLeftTradable) {
    // absent api state fields leave the contract tradable
    static thread_local ContractBuffer storage;
    const auto rows = parse_contracts(R"({"code":0,"msg":"","data":[{"symbol":"X-USDT","status":1,"pricePrecision":2,)"
                                      R"("quantityPrecision":1,"size":"0.1","tradeMinQuantity":"0.1",)"
                                      R"("tradeMinUSDT":"2"}]})",
                                      storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_TRUE((*rows)[0].api_open);
    EXPECT_TRUE((*rows)[0].api_close);
}

TEST(BingxWire, RefusesAnEnvelopeWithoutACode) {
    EXPECT_FALSE(parse_rest_envelope(R"({"msg":"","data":[]})").has_value());
    EXPECT_FALSE(parse_rest_envelope(R"({"code":"0","msg":""})").has_value());
    EXPECT_FALSE(parse_rest_envelope("not json").has_value());
}

TEST(BingxWire, ParsesContracts) {
    static thread_local ContractBuffer storage;
    const auto body = load("rest_contracts.json");
    const auto rows = parse_contracts(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 5U);

    EXPECT_EQ((*rows)[0].symbol, "BTC-USDT");
    EXPECT_EQ((*rows)[0].status, 1);
    EXPECT_EQ((*rows)[0].price_precision, 1);
    EXPECT_EQ((*rows)[0].quantity_precision, 4);
    EXPECT_EQ((*rows)[0].trade_min_usdt, "2");

    // size contradicts the precision on this row; the precision matches live quantities
    EXPECT_EQ((*rows)[2].symbol, "SOL-USDT");
    EXPECT_EQ((*rows)[2].size, "\"1\"");
    EXPECT_EQ((*rows)[2].quantity_precision, 2);

    EXPECT_EQ((*rows)[3].status, 25);
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

TEST(BingxMap, DerivesStepsAsExactPowersOfTen) {
    EXPECT_EQ(step_from_precision(0)->raw, k_e9);        // 1
    EXPECT_EQ(step_from_precision(1)->raw, 100'000'000); // 0.1
    EXPECT_EQ(step_from_precision(4)->raw, 100'000);     // 0.0001
    EXPECT_EQ(step_from_precision(8)->raw, 10);          // 0.00000001
    EXPECT_EQ(step_from_precision(9)->raw, 1);
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
    EXPECT_EQ(std::get<PerpInstrument>(*btc).funding_interval_s, 0U);

    const auto sol = map_instrument((*rows)[2], MarketType::Perp);
    ASSERT_TRUE(sol.has_value());
    EXPECT_EQ(base_of(*sol).tick_size.raw, 1'000'000); // 10^-3
    EXPECT_EQ(base_of(*sol).lot_size.raw, 10'000'000); // 10^-2, not `size` of 1

    // status 25: listed but not trading
    EXPECT_FALSE(map_instrument((*rows)[3], MarketType::Perp).has_value());
    EXPECT_FALSE(map_instrument((*rows)[0], MarketType::Spot).has_value());
}

TEST(BingxMap, TheCanonicalPairIsSplitOutOfTheBingxSymbol) {
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

    for (const char* symbol : {"BTCUSDT", "BTC-USDT-SWAP", "-USDT", "BTC-", ""}) {
        wire.symbol = symbol;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << symbol;
    }

    // a fourteen-character base must load
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
    // T is when the book changed
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
    EXPECT_EQ(book->bid_count, k_bingx_depth);
    EXPECT_EQ(book->ask_count, k_bingx_depth);
    static_assert(k_bingx_depth <= k_book_levels);
    EXPECT_EQ(book->venue_seq, 0U); // the depth channel carries no sequence number
    EXPECT_EQ(book->ts_event.ns, 1786983056064ULL * 1'000'000ULL);
    EXPECT_EQ(book->ts_init.ns, 99U);

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

TEST(BingxMap, APerpTouchIsTheTopOfItsLadder) {
    EXPECT_EQ(data_type_suffix(exec::SubKind::Bbo, MarketType::Perp),
              data_type_suffix(exec::SubKind::Depth, MarketType::Perp));
    EXPECT_EQ(data_type_suffix(exec::SubKind::Bbo, MarketType::Spot), "bookTicker");

    const auto table = catalogue();
    LevelBuffer storage;
    const auto frame = load("ws_depth100.json");
    const auto wire = parse_depth(frame, storage, MarketType::Perp);
    ASSERT_TRUE(wire.has_value());
    const auto book = map_book(*wire, table, MarketType::Perp, UnixNanos{99});
    ASSERT_TRUE(book.has_value());
    const auto touch = touch_of(*book);
    ASSERT_TRUE(touch.has_value());
    EXPECT_EQ(touch->instrument.packed, base_for(table, "BTC-USDT").id.packed);
    EXPECT_EQ(touch->bid.raw, book->bids[0].price.raw);
    EXPECT_EQ(touch->ask.raw, book->asks[0].price.raw);
    EXPECT_EQ(touch->bid_qty.raw, book->bids[0].qty.raw);
    EXPECT_EQ(touch->ask_qty.raw, book->asks[0].qty.raw);
    EXPECT_LT(touch->bid.raw, touch->ask.raw);
    EXPECT_EQ(touch->ts_event.ns, 1786983056064ULL * 1'000'000ULL);

    auto one_sided = *book;
    one_sided.ask_count = 0;
    EXPECT_FALSE(touch_of(one_sided).has_value());
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

    // m false: the buyer took the offer
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
    EXPECT_EQ(funding->interval_s, 8U * 3600U);
    // bingx gives no forecast
    EXPECT_TRUE(funding->predicted_rate.is_undef());
    EXPECT_EQ(funding->ts_event.ns, 0U);

    WirePremiumIndex silent = *wire;
    silent.last_funding_rate = {};
    EXPECT_FALSE(map_funding(silent, table, UnixNanos{1}).has_value());
}

TEST(BingxWs, BuildsSubscriptionFrames) {
    Protocol protocol;
    const auto first = protocol.subscribe_frame("BTC-USDT", data_type_suffix(exec::SubKind::Bbo, MarketType::Perp));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text, R"({"id":"1","reqType":"sub","dataType":"BTC-USDT@depth100@100ms"})");

    const auto second = protocol.subscribe_frame("ETH-USDT", data_type_suffix(exec::SubKind::Trades, MarketType::Perp));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text, R"({"id":"2","reqType":"sub","dataType":"ETH-USDT@trade"})");

    const auto depth = protocol.subscribe_frame("BTC-USDT", data_type_suffix(exec::SubKind::Depth, MarketType::Perp));
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->text, R"({"id":"3","reqType":"sub","dataType":"BTC-USDT@depth100@100ms"})");

    const auto off = protocol.unsubscribe_frame("BTC-USDT", "bookTicker");
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(off->text, R"({"id":"4","reqType":"unsub","dataType":"BTC-USDT@bookTicker"})");

    EXPECT_EQ(Protocol::pong_frame(), "Pong");
}

TEST(BingxWs, RefusesToBuildAFrameItCannotName) {
    Protocol protocol;
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
    EXPECT_EQ(builder.premium_index("BTC-USDT").target, "/openApi/swap/v2/quote/premiumIndex?symbol=BTC-USDT");
    EXPECT_EQ(builder.depth("BTC-USDT", 5).target, "/openApi/swap/v2/quote/depth?symbol=BTC-USDT&limit=5");
    EXPECT_EQ(builder.ticker("ETH-USDT").target, "/openApi/swap/v2/quote/ticker?symbol=ETH-USDT");
    EXPECT_EQ(builder.contracts().method, net::HttpMethod::Get);
}

// a ulid and the lowercase form bingx echoes
constexpr std::string_view k_ulid = "01JD7Q0M9WKE6Q3N2F4H8ZT5XR";
constexpr std::string_view k_ulid_echo = "01jd7q0m9wke6q3n2f4h8zt5xr";
// spot echoes this one unchanged
constexpr std::string_view k_ulid_spot = "01JD7Q0M9WKE6Q3N2F4H8ZT5XT";

ExchangeClientOrderId ulid(std::string_view text = k_ulid) {
    const auto id = ExchangeClientOrderId::from(text);
    EXPECT_TRUE(id.has_value());
    return id.value_or(ExchangeClientOrderId{});
}

TEST(BingxClientId, CarriesAUlidWholeAndUndoesTheVenuesLowercasing) {
    EXPECT_EQ(client_id_text(ulid()), k_ulid);
    const auto decoded = client_id_from_text(k_ulid_echo);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->view(), k_ulid);
    EXPECT_EQ(client_id_from_text(k_ulid)->view(), k_ulid);
}

TEST(BingxClientId, RefusesAnythingThatIsNotOneOfOurs) {
    EXPECT_FALSE(client_id_from_text("myOrder123").has_value());
    EXPECT_FALSE(client_id_from_text("").has_value());
    // i, l, o and u are not in crockford base32
    EXPECT_FALSE(client_id_from_text("01JD7Q0M9WKE6Q3N2F4H8ZT5XI").has_value());
    EXPECT_TRUE(client_id_text(ulid("SHORT")).empty());
}

// fixed test pair; expected digests computed with openssl dgst -sha256 -hmac
constexpr std::string_view k_key = "bingx-test-key";
constexpr std::string_view k_secret = "bingx-test-secret";
constexpr UnixNanos k_now{1'700'000'000'000ULL * 1'000'000ULL};

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

TEST(BingxSpotWire, TheSpotBookArrivesWithItsBestOfferLast) {
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
    TradeBuffer storage;
    const auto frame = load("ws_spot_trade.json");
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].symbol, "BTC-USDT");
    EXPECT_FALSE((*trades)[0].price.empty());
    EXPECT_NE((*trades)[0].trade_ms, 0U);
}

TEST(BingxSpotWire, TheSpotQuoteCarriesNoSequenceAndSaysSo) {
    // spot omits u and T; zero means absent
    const auto frame = load("ws_spot_book_ticker.json");
    const auto quote = parse_book_ticker(frame);
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->symbol, "BTC-USDT");
    EXPECT_EQ(quote->update_id, 0U);
    EXPECT_EQ(quote->transaction_ms, 0U);
    EXPECT_NE(quote->event_ms, 0U);
}

TEST(BingxSpotWire, TheSpotTickerSendsTheSameColumnsUnquoted) {
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
    EXPECT_EQ(stats->volume_24h_quote.raw, 254'483'206'580'000LL);
}

TEST(BingxSpotMap, ASpotFrameResolvesAgainstTheSpotCatalogue) {
    const auto table = spot_catalogue();
    const auto frame = load("ws_spot_book_ticker.json");
    const auto quote = parse_book_ticker(frame);
    ASSERT_TRUE(quote.has_value());

    EXPECT_FALSE(map_bbo(*quote, table, MarketType::Perp, UnixNanos{1}).has_value());

    const auto bbo = map_bbo(*quote, table, MarketType::Spot, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->instrument.market(), MarketType::Spot);
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
}

OrderCmd limit_buy(const exec::InstrumentTable& table, std::string_view symbol, std::int64_t ticks, std::int64_t lots) {
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

// the hashed bytes, which are also the bytes sent
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
    // bingx's documented example; the digest came from openssl
    std::array<char, net::k_hex_bytes> out{};
    const auto hex = net::hmac_sha256_hex("SECRET_KEY", "recvWindow=0&symbol=BTC-USDT&timestamp=1696751141337", out);
    EXPECT_EQ(hex, "fe041f159118c90ac13eab4d32f9e2d75b80ca6fe17ca8acd290aba864753ce2");
}

TEST(BingxSign, PerpetualCreateSignsTheStringItSends) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(table, "BTC-USDT", 444'596, 30), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    constexpr std::string_view
        k_canonical = "clientOrderID=01JD7Q0M9WKE6Q3N2F4H8ZT5XR&clientOrderId=01JD7Q0M9WKE6Q3N2F4H8ZT5XR"
                      "&positionSide=BOTH&price=44459.6&quantity=0.003&recvWindow=5000&side=BUY"
                      "&symbol=BTC-USDT&timeInForce=GTC&timestamp=1699999999500&type=LIMIT";
    EXPECT_EQ(builder.last_sign_message(), k_canonical);
    EXPECT_EQ(query_of(request->target).substr(0, k_canonical.size()), k_canonical);
    EXPECT_EQ(request->target,
              std::string{"/openApi/swap/v2/trade/order?"} + std::string{k_canonical} +
                  "&signature=4a5476229548e75de93fce60a00e998fd2dbcb83c8eed681f232ea46ead4e068");
    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_TRUE(request->body.empty()); // parameters ride the query
    ASSERT_EQ(request->headers.size(), 1U);
    EXPECT_EQ(request->headers[0].name, "X-BX-APIKEY");
    EXPECT_EQ(request->headers[0].value, k_key);
}

TEST(BingxSign, SpotCreateSignsItsOwnCanonicalString) {
    const auto table = spot_catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto request = builder.create_order(limit_buy(table, "BTC-USDT", 4'445'960, 3'000), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    constexpr std::string_view k_canonical = "newClientOrderId=01JD7Q0M9WKE6Q3N2F4H8ZT5XR&price=44459.6&quantity=0.003"
                                             "&recvWindow=5000&side=BUY&symbol=BTC-USDT&timeInForce=GTC"
                                             "&timestamp=1699999999500&type=LIMIT";
    EXPECT_EQ(builder.last_sign_message(), k_canonical);
    EXPECT_EQ(request->target,
              std::string{"/openApi/spot/v1/trade/order?"} + std::string{k_canonical} +
                  "&signature=774b96e3aa2d2ea8e123d674857108593feecbaca98c14811f0c00643c992054");
    EXPECT_EQ(request->method, net::HttpMethod::Post);
}

TEST(BingxSign, EveryCanonicalStringIsAsciiSorted) {
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
    EXPECT_TRUE(wallet->target.starts_with("/openApi/swap/v3/user/balance?"));

    const auto book = builder.funding_income(k_since, k_now);
    ASSERT_TRUE(book.has_value());
    EXPECT_TRUE(book->target.starts_with("/openApi/swap/v2/user/income?"));
    EXPECT_NE(builder.last_sign_message().find("startTime=1702713600000"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("incomeType="), std::string_view::npos);

    ASSERT_TRUE(builder.funding_income(UnixNanos{}, k_now).has_value());
    EXPECT_EQ(builder.last_sign_message().find("startTime="), std::string_view::npos);
}

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
    EXPECT_NE(builder.last_sign_message().find("clientOrderID=01JD"), std::string_view::npos);
    EXPECT_NE(builder.last_sign_message().find("clientOrderId=01JD"), std::string_view::npos);

    auto spot_cancel_cmd = limit_buy(spot, "BTC-USDT", 4'445'960, 3'000);
    spot_cancel_cmd.kind = CmdKind::Cancel;
    const auto spot_cancel = builder.cancel_order(spot_cancel_cmd, spot, k_now);
    ASSERT_TRUE(spot_cancel.has_value());
    EXPECT_EQ(spot_cancel->method, net::HttpMethod::Post);
    EXPECT_TRUE(spot_cancel->target.starts_with("/openApi/spot/v1/trade/cancel?"));
    EXPECT_NE(builder.last_sign_message().find("clientOrderID=01JD"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("clientOrderId=01JD"), std::string_view::npos);

    ASSERT_TRUE(builder.open_orders(MarketType::Spot, k_now).has_value());
    EXPECT_TRUE(builder.open_orders(MarketType::Spot, k_now)->target.starts_with("/openApi/spot/v1/trade/openOrders?"));
    EXPECT_TRUE(builder.open_orders(MarketType::Perp, k_now)->target.starts_with("/openApi/swap/v2/trade/openOrders?"));
    EXPECT_TRUE(builder.positions(MarketType::Perp, k_now)->target.starts_with("/openApi/swap/v2/user/positions?"));
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
    EXPECT_NE(builder.last_sign_message().find("orderId=1585839271162413056"), std::string_view::npos);
    EXPECT_EQ(builder.last_sign_message().find("clientOrderId="), std::string_view::npos);

    cmd.target_exchange_order_id = {};
    cmd.exchange_client_order_id = {};
    EXPECT_EQ(builder.cancel_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
}

TEST(BingxSign, HedgeModeChangesThePositionSideAndDropsReduceOnly) {
    const auto table = catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    EXPECT_FALSE(builder.hedge_mode()); // bingx's default

    auto cmd = limit_buy(table, "BTC-USDT", 444'596, 30);
    cmd.side = Side::Sell;
    cmd.flags = OrderFlags::ReduceOnly;
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
    EXPECT_NE(builder.last_sign_message().find("positionSide=BOTH"), std::string_view::npos);
    EXPECT_NE(builder.last_sign_message().find("reduceOnly=true"), std::string_view::npos);

    builder.set_hedge_mode(true);
    ASSERT_TRUE(builder.create_order(cmd, table, k_now).has_value());
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
    EXPECT_EQ(builder.positions(MarketType::Spot, k_now).error(), BuildError::SpotPositionsUnsupported);
}

TEST(BingxSign, RefusesAnAmendItCannotExpress) {
    const auto perp = catalogue();
    const auto spot = spot_catalogue();
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

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
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("webOrder1");
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::NoOrderTarget);
    cmd.exchange_client_order_id = ulid();
    cmd.qty = QtyLots::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::BadQuantity);
    cmd.qty = QtyLots::from_raw(30);
    cmd.price = PriceTicks::from_raw(0);
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::BadPrice);

    EXPECT_EQ(builder.listen_key_keepalive("abc&symbol=ETH-USDT", k_now).error(), BuildError::UnsafeParameter);
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
    EXPECT_EQ(private_stream_host(MarketType::Perp), "open-api-swap.bingx.com");
    EXPECT_EQ(private_stream_host(MarketType::Spot), "open-api-ws.bingx.com");
    EXPECT_EQ(private_stream_target(MarketType::Perp, "abc"), "/swap-market?listenKey=abc");
    EXPECT_EQ(private_stream_target(MarketType::Spot, "abc"), "/market?listenKey=abc");

    Protocol protocol;
    const auto frame = protocol.subscribe_channel("spot.executionReport");
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->text, R"({"id":"1","reqType":"sub","dataType":"spot.executionReport"})");
}

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
    EXPECT_EQ((*perp)[0].status, WireOrderStatus::New);
    EXPECT_EQ((*perp)[0].price, "44459.6");
    EXPECT_EQ((*perp)[1].status, WireOrderStatus::PartiallyFilled);
    EXPECT_EQ((*perp)[1].executed_qty, "0.40");

    const auto spot_body = load("rest_open_orders_spot.json");
    static thread_local OrderBuffer spot_storage;
    const auto spot = parse_orders(spot_body, spot_storage);
    ASSERT_TRUE(spot.has_value()) << to_string(spot.error());
    ASSERT_EQ(spot->size(), 1U);
    EXPECT_EQ((*spot)[0].client_order_id, k_ulid_spot);
    EXPECT_EQ((*spot)[0].status, WireOrderStatus::New);
}

TEST(BingxWire, ReadsAnAckWhicheverWayItIsWrapped) {
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

    const auto bare = load("rest_listen_key.json");
    const auto wrapped = load("rest_listen_key_wrapped.json");
    ASSERT_TRUE(parse_listen_key(bare).has_value());
    ASSERT_TRUE(parse_listen_key(wrapped).has_value());
    EXPECT_EQ(*parse_listen_key(bare), *parse_listen_key(wrapped));

    const auto mode_body = load("rest_position_mode.json");
    const auto mode = parse_position_mode(mode_body);
    ASSERT_TRUE(mode.has_value());
    EXPECT_TRUE(*mode); // a string, not a json bool
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
    const auto perp_body = load("ws_order_update_perp_fill.json");
    const auto perp = parse_order_update(perp_body);
    ASSERT_TRUE(perp.has_value()) << to_string(perp.error());
    EXPECT_EQ(perp->symbol, "BTC-USDT");
    EXPECT_EQ(perp->client_order_id, k_ulid_echo);
    EXPECT_EQ(perp->exec_type, "TRADE");
    EXPECT_EQ(perp->last_qty, "0.0012");
    EXPECT_EQ(perp->last_price, "44459.6");
    EXPECT_EQ(perp->fee, "-0.02667576");
    EXPECT_FALSE(perp->maker_known); // the perp push does not say
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
    EXPECT_TRUE(spot->client_order_id.empty()); // the memo covers this
}

TEST(BingxWire, ReadsBothSpellingsOfCancelled) {
    EXPECT_EQ(order_status_of("CANCELED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status_of("CANCELLED"), WireOrderStatus::Canceled);
    EXPECT_EQ(order_status_of("Pending"), WireOrderStatus::New);
    EXPECT_EQ(order_status_of("FAILED"), WireOrderStatus::Rejected);
    EXPECT_EQ(order_status_of("Unknown"), WireOrderStatus::Unknown);
    EXPECT_EQ(order_status_of(""), WireOrderStatus::Unknown);
}

TEST(BingxMap, BuildsTheSpotCatalogueFromStatedSteps) {
    const auto table = spot_catalogue();
    // offline, api-closed and 1e-10-tick rows are dropped
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
    EXPECT_EQ(resting->exchange_client_order_id.view(), k_ulid);
    EXPECT_EQ(resting->exchange_order_id.view(), "1585839271162413056");

    const auto partial = map_open_order((*rows)[1], table, MarketType::Perp);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(partial->qty.raw, 100);   // 1.00 at a lot of 0.01
    EXPECT_EQ(partial->leaves.raw, 60); // 1.00 - 0.40
}

TEST(BingxMap, RefusesARowFromTheOtherMarket) {
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
    const auto fill = map_order_update(*fill_wire, table, MarketType::Perp, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    // the fill's own size and price: 0.0012 at 44459.6 of 0.0030
    EXPECT_EQ(fill->qty.raw, 12);
    EXPECT_EQ(fill->price.raw, 444'596);
    EXPECT_EQ(fill->leaves_qty.raw, 18);
    // "-0.02667576" on the wire is a charge
    EXPECT_EQ(fill->fee.raw, 26'675'760);
    EXPECT_EQ(fill->liquidity, Liquidity::Unknown); // the push does not say
    EXPECT_TRUE(fill->exec_id.empty());             // no trade id

    const auto cancel_body = load("ws_order_update_perp_cancel.json");
    const auto cancel_wire = parse_order_update(cancel_body);
    ASSERT_TRUE(cancel_wire.has_value());
    const auto cancel = map_order_update(*cancel_wire, table, MarketType::Perp, ExchangeClientOrderId{}, UnixNanos{7});
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->type, OrderEventType::Canceled);
    EXPECT_EQ(cancel->leaves_qty.raw, 18);
}

TEST(BingxMap, MapsASpotFillThroughTheMemo) {
    const auto table = spot_catalogue();
    const auto body = load("ws_order_update_spot_fill.json");
    const auto wire = parse_order_update(body);
    ASSERT_TRUE(wire.has_value());

    const auto orphan = map_order_update(*wire, table, MarketType::Spot, ExchangeClientOrderId{}, UnixNanos{7});
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

    const auto short_side = map_position((*rows)[1], table);
    ASSERT_TRUE(short_side.has_value());
    EXPECT_EQ(short_side->net_qty.raw, -20);
}

TEST(BingxErrors, ClassifiesWhatTheVenueActuallySaid) {
    EXPECT_EQ(classify(k_bad_api_key), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_ws_data_type_unsupported), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_symbol_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_parameter), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_endpoint_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(999'999), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(0), exec::ErrorClass::Terminal);

    EXPECT_EQ(classify(k_bad_signature), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_missing_signature), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_no_permission), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_ip_not_whitelisted), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify(k_kyc_required), exec::ErrorClass::AuthFailure);

    EXPECT_EQ(classify(k_rate_limited), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_risk_control), exec::ErrorClass::RateLimited);

    EXPECT_EQ(classify(k_bad_timestamp), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_funding_settling), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_order_system_busy), exec::ErrorClass::Retryable);
}

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
    // negative: the account paid
    EXPECT_EQ((*rows)[0].income, "-0.0292");
    EXPECT_EQ((*rows)[0].time_ms, 1'702'713'615'000ULL);
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
    EXPECT_EQ(report->available.raw, 193'760'900'000);
    EXPECT_EQ(report->unrealized.raw, 1'921'900'000);
    EXPECT_EQ(report->ts_venue.ns, k_now.ns);
    // equity (196.7431) includes open pnl
    EXPECT_NE(report->total.raw, 196'743'100'000);
}

TEST(BingxMap, BalanceLeavesAFieldBingxDidNotSendUndef) {
    WireBalance wire{};
    wire.asset = "USDT";
    wire.balance = "1200.0000";
    const auto report = map_balance(wire, k_now);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->total.raw, 1'200'000'000'000);
    EXPECT_TRUE(report->available.is_undef());
    EXPECT_TRUE(report->unrealized.is_undef());
}

TEST(BingxMap, BalanceWithoutAnAssetIsDropped) {
    WireBalance wire{};
    wire.balance = "12.5000";
    EXPECT_FALSE(map_balance(wire, k_now).has_value());
    wire.asset = "TWENTYONECHARACTERSXX";
    EXPECT_FALSE(map_balance(wire, k_now).has_value());
}

TEST(BingxErrors, ReadsARefusalOutOfABodyThatCameBackWithHttp200) {
    // http 200 with 100413 in the body
    const auto body = load("rest_error_bad_key.json");
    const auto envelope = parse_rest_envelope(body);
    ASSERT_TRUE(envelope.has_value());
    EXPECT_EQ(envelope->code, k_bad_api_key);
    EXPECT_EQ(classify(envelope->code), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_http(200), exec::ErrorClass::Terminal); // the envelope decides
}

TEST(BingxErrors, ForgivesACancelThatLostItsRace) {
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

TEST(BingxWire, ReadsFieldsInAnyOrder) {
    LevelBuffer levels;
    const auto depth = parse_depth(R"({"data":{"asks":[["2","1"]],"x":[[1]],"bids":[["1","3"]]},"timestamp":5,)"
                                   R"("dataType":"ETH-USDT@depth5"})",
                                   levels,
                                   MarketType::Perp);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->symbol, "ETH-USDT");
    EXPECT_EQ(depth->ts_ms, 5U) << "no ts, so the spot stamp";
    ASSERT_EQ(depth->bids.size(), 1U);
    ASSERT_EQ(depth->asks.size(), 1U);
    EXPECT_EQ(depth->asks[0].price, "2");

    TradeBuffer trades;
    const auto trade = parse_trades(R"({"data":{"m":true,"T":4,"s":"ETH-USDT","q":"1","p":"2"},"code":0})", trades);
    ASSERT_TRUE(trade.has_value());
    ASSERT_EQ(trade->size(), 1U);
    EXPECT_EQ((*trade)[0].symbol, "ETH-USDT");
    EXPECT_EQ((*trade)[0].trade_ms, 4U);
    EXPECT_TRUE((*trade)[0].buyer_is_maker);
    EXPECT_FALSE(parse_trades(R"({"data":[{"p":"2","q":"1","T":4}]})", trades).has_value()) << "no symbol";

    const auto top = parse_book_ticker(R"({"data":{"A":"4","a":"3","B":"2","b":"1","s":"ETH-USDT","E":7,"u":6}})");
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->ask_qty, "4");
    EXPECT_EQ(top->update_id, 6U);
    EXPECT_EQ(top->transaction_ms, 0U);
    EXPECT_EQ(top->event_ms, 7U);
}

} // namespace
} // namespace raikiri::venues::bingx
