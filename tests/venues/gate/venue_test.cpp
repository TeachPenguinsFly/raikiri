#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/errors.hpp>
#include <raikiri/venues/gate/gate_venue.hpp>
#include <raikiri/venues/gate/map.hpp>
#include <raikiri/venues/gate/rest.hpp>
#include <raikiri/venues/gate/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::gate {
namespace {

// The depth the adapter subscribes at; gate deletes what falls out of it.
constexpr std::uint32_t k_gate_window = 20;

// There is no RAIKIRI_GATE_FIXTURES define: this target is the venue test, so
// the fixture directory is located from this file rather than from CMake.
std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
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

TEST(GateVenue, RegistersAndBuilds) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const auto adapter =
        exec::make_venue({.venue = VenueId::Gate, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    EXPECT_EQ(adapter->caps.venue, VenueId::Gate);
    EXPECT_NE(adapter->data, nullptr);
    EXPECT_NE(adapter->execution, nullptr);
    EXPECT_NE(adapter->instruments, nullptr);
}

TEST(GateVenue, OrderWithoutAnImplementationFailsLoudly) {
    // Silence would leave the order open in the store forever.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Gate, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());

    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = InstrumentId::make(VenueId::Gate, MarketType::Perp, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    adapter->execution->submit(cmd);
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);

    cmd.kind = CmdKind::Cancel;
    adapter->execution->cancel(cmd);
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
}

TEST(GateVenue, SpotIsRefusedRatherThanReturnedEmpty) {
    // An empty catalogue reads as "this venue lists nothing", which is a very
    // different claim from "this adapter does not cover spot".
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Gate, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    const auto loaded = adapter->instruments->fetch_all(MarketType::Spot);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().cls, exec::ErrorClass::Terminal);
}

// ------------------------------------------------------------- wire ----

TEST(GateWire, EventDispatchResolvesKindFromChannelAndEvent) {
    for (const auto& [file, kind] :
         {std::pair{"ws_book_ticker.json", EventKind::BookTicker},
          std::pair{"ws_trades.json", EventKind::Trade},
          std::pair{"ws_tickers.json", EventKind::Ticker},
          std::pair{"ws_order_book_update.json", EventKind::BookUpdate}}) {
        const auto frame = fixture(file);
        const auto event = parse_event(frame);
        ASSERT_TRUE(event.has_value()) << file;
        EXPECT_EQ(event->kind, kind) << file;
        // Data frames carry the contract in four different places; their own
        // parsers report it rather than paying for the descent twice.
        EXPECT_TRUE(event->contract.empty()) << file;
    }
}

TEST(GateWire, ControlFramesAreNotMistakenForData) {
    // Ack and refusal differ only by the presence of an `error` object — both
    // keep event:"subscribe". Reading a refusal as an ack turns "the venue
    // rejected this channel" into "this channel is quiet", which is
    // indistinguishable from a market with no activity.
    const auto ack = fixture("ws_subscribe_ack.json");
    auto event = parse_event(ack);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::SubscribeAck);
    EXPECT_EQ(event->contract, "BTC_USDT");

    const auto bad = fixture("ws_error.json");
    event = parse_event(bad);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Error);
    EXPECT_EQ(event->contract, "NOPE_USDT");

    const auto err = parse_stream_error(bad);
    ASSERT_TRUE(err.has_value());
    // A NUMBER here, unlike the string label a REST refusal carries.
    EXPECT_EQ(err->code, k_ws_invalid_argument);
    EXPECT_FALSE(err->message.empty());
}

TEST(GateWire, KeepaliveAnswerIsItsOwnChannelWithAnEmptyEvent) {
    // futures.pong carries event:"" and result:null, so dispatching on the
    // event alone would file it as an unknown control frame.
    const auto frame = fixture("ws_pong.json");
    const auto event = parse_event(frame);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Pong);
}

TEST(GateWire, BookTickerParsesBothSidesAndKeepsSizesUnquoted) {
    const auto frame = fixture("ws_book_ticker.json");
    const auto book = parse_book_ticker(frame);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->contract, "BTC_USDT");
    EXPECT_EQ(book->bid_price, "64155.8");
    EXPECT_EQ(book->ask_price, "64155.9");
    // Quoted price, BARE INTEGER size, in the same object. A reader that
    // demanded quotes would drop every quantity Gate sends.
    EXPECT_EQ(book->bid_qty, "76800");
    EXPECT_EQ(book->ask_qty, "535");
    EXPECT_GT(book->update_id, 0U);
    // `t` is milliseconds here even though most other Gate epoch fields are
    // seconds; reading it as seconds would place the tick in the year 58000.
    EXPECT_GT(book->ts_ms, 1'700'000'000'000U);
    EXPECT_LT(book->ts_ms, 100'000'000'000'000U);

    // Views point into the caller's buffer, not into parser scratch.
    EXPECT_GE(book->bid_qty.data(), frame.data());
    EXPECT_LE(book->bid_qty.data() + book->bid_qty.size(), frame.data() + frame.size());
}

TEST(GateWire, TradeSignCarriesTheAggressorSide) {
    // Gate names no side at all: the sign of `size` is the taker direction and
    // the magnitude is the quantity. Reading `size` as an unsigned number
    // flattens every signed flow series to one direction.
    auto storage = std::make_unique<TradeBuffer>();
    const auto sell_frame = fixture("ws_trades.json");
    const auto sells = parse_trades(sell_frame, *storage);
    ASSERT_TRUE(sells.has_value());
    ASSERT_FALSE(sells->empty());
    const auto& sell = sells->front();
    EXPECT_EQ(sell.contract, "BTC_USDT");
    EXPECT_FALSE(sell.aggressor_bought);
    EXPECT_FALSE(sell.price.empty());
    // The minus is stripped, and what is left is still a view into the frame.
    EXPECT_EQ(sell.qty.front() == '-', false);
    EXPECT_GE(sell.qty.data(), sell_frame.data());
    EXPECT_GT(sell.trade_id, 0);
    EXPECT_GT(sell.ts_ms, 1'700'000'000'000U);

    auto buy_storage = std::make_unique<TradeBuffer>();
    const auto buy_frame = fixture("ws_trades_buy.json");
    const auto buys = parse_trades(buy_frame, *buy_storage);
    ASSERT_TRUE(buys.has_value());
    ASSERT_FALSE(buys->empty());
    EXPECT_TRUE(buys->front().aggressor_bought);
}

TEST(GateWire, TickerCarriesFundingAndMarkOnOneChannel) {
    const auto frame = fixture("ws_tickers.json");
    const auto ticker = parse_ticker(frame);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->contract, "BTC_USDT");
    EXPECT_FALSE(ticker->funding_rate.empty());
    // Unlike okx, one subscription answers both questions.
    EXPECT_FALSE(ticker->mark_price.empty());
    EXPECT_EQ(ticker->funding_interval_s, 28'800U);
    // SECONDS, and small enough that a millisecond reading is impossible.
    EXPECT_GT(ticker->funding_next_apply_s, 1'700'000'000U);
    EXPECT_LT(ticker->funding_next_apply_s, 10'000'000'000U);
    // `t` on the same object is milliseconds. The two units live side by side.
    EXPECT_GT(ticker->ts_ms, 1'700'000'000'000U);
    // Deprecated per Gate's own SDK and observed equal to funding_rate, which
    // is why the mapper refuses to publish it as a forecast.
    EXPECT_EQ(ticker->funding_rate_indicative, ticker->funding_rate);
}

TEST(GateWire, BookUpdateChainsOnUpdateIds) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto first_frame = fixture("ws_order_book_update.json");
    const auto first = parse_book_update(first_frame, *storage);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->contract, "BTC_USDT");
    EXPECT_LE(first->first_update_id, first->last_update_id);

    auto next_storage = std::make_unique<LevelBuffer>();
    const auto second_frame = fixture("ws_order_book_update_next.json");
    const auto second = parse_book_update(second_frame, *next_storage);
    ASSERT_TRUE(second.has_value());
    // The chaining rule: U is one past the previous message's u. These are
    // consecutive frames off one live socket. Gate sends no snapshot on this
    // channel, so a gap here means re-reading the REST book, not a resync
    // against anything the stream itself provides.
    EXPECT_EQ(second->first_update_id, first->last_update_id + 1);
}

TEST(GateWire, DeletedLevelsKeepTheirZeroQuantity) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_order_book_update_next.json");
    const auto update = parse_book_update(frame, *storage);
    ASSERT_TRUE(update.has_value());
    bool saw_delete = false;
    for (const auto& level : update->bids) {
        EXPECT_FALSE(level.price.empty());
        EXPECT_FALSE(level.qty.empty()); // "0" is a delete, never an absence
        saw_delete = saw_delete || level.qty == "0";
    }
    EXPECT_TRUE(saw_delete);
}

TEST(GateWire, OrderBookIsOnlyASeedWhenItCarriesAnId) {
    auto storage = std::make_unique<LevelBuffer>();
    const auto seed = parse_order_book(fixture("rest_order_book_seed.json"), *storage);
    ASSERT_TRUE(seed.has_value());
    EXPECT_EQ(seed->update_id, 121'670'616'851ULL);
    EXPECT_EQ(seed->update_s, 1'786'831'342ULL) << "fractional seconds, truncated";
    EXPECT_EQ(seed->bids.size(), 20U);
    EXPECT_EQ(seed->asks.size(), 20U);

    // Same endpoint without with_id=true. The levels are fine and the body is
    // useless: nothing says where in the chain they sit.
    auto plain_storage = std::make_unique<LevelBuffer>();
    EXPECT_FALSE(parse_order_book(fixture("rest_order_book.json"), *plain_storage).has_value());
}

TEST(GateWire, ContractsAreATopLevelArray) {
    // No envelope: success is a bare JSON array, which is why the session has
    // to be able to open a document as either shape.
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 4U);

    const auto find = [&](std::string_view name) -> const WireContract* {
        for (const auto& row : *rows) {
            if (row.name == name) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto* btc = find("BTC_USDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->status, "trading");
    EXPECT_EQ(btc->order_price_round, "0.1");
    EXPECT_EQ(btc->quanto_multiplier, "0.0001"); // one contract is 0.0001 BTC
    EXPECT_FALSE(btc->in_delisting);
    EXPECT_EQ(btc->funding_interval_s, 28'800U);
    // Gate's own SDK documents order_size_min as a string; the live endpoint
    // sends an integer. Scalars are read from the raw token for exactly this.
    EXPECT_EQ(btc->order_size_min, "1");

    const auto* eth = find("ETH_USDT");
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(eth->quanto_multiplier, "0.01");

    const auto* delisting = find("DOGE_USDT");
    ASSERT_NE(delisting, nullptr);
    EXPECT_TRUE(delisting->in_delisting);
    EXPECT_EQ(delisting->status, "delisting");
}

TEST(GateWire, ServerTimeIsMilliseconds) {
    const auto time = parse_server_time(fixture("rest_server_time.json"));
    ASSERT_TRUE(time.has_value());
    EXPECT_GT(*time, 1'700'000'000'000U);
}

TEST(GateWire, RestRefusalIsAStringLabelAndMayCarryNoMessage) {
    const auto bare = fixture("rest_error_contract_not_found.json");
    const auto err = parse_rest_error(bare);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->label, k_contract_not_found);
    // Observed live: this refusal ships the label alone, so requiring a
    // message would turn a classifiable error into an unreadable one.
    EXPECT_TRUE(err->message.empty());

    const auto detailed = fixture("rest_error_invalid_param.json");
    const auto with_message = parse_rest_error(detailed);
    ASSERT_TRUE(with_message.has_value());
    EXPECT_EQ(with_message->label, k_invalid_param_value);
    EXPECT_FALSE(with_message->message.empty());
}

TEST(GateWire, SuccessBodiesAreNotReadAsRefusals) {
    // A body with no label is a success, and mistaking one for the other in
    // either direction loses a catalogue or invents an outage.
    EXPECT_FALSE(parse_rest_error(fixture("rest_contracts.json")).has_value());
    EXPECT_FALSE(parse_rest_error(fixture("rest_server_time.json")).has_value());
    EXPECT_FALSE(parse_rest_error(fixture("rest_order_book.json")).has_value());
}

TEST(GateWire, GarbageIsRefusedRatherThanPartiallyRead) {
    auto levels = std::make_unique<LevelBuffer>();
    auto trades = std::make_unique<TradeBuffer>();
    auto contracts = std::make_unique<ContractBuffer>();
    for (const char* junk : {"",
                             "{",
                             "[]",
                             "null",
                             R"({"channel":)",
                             "\xff\xfe",
                             R"({"channel":"futures.book_ticker","event":"update"})",
                             R"({"channel":"futures.book_ticker","event":"update","result":{}})"}) {
        const std::string_view frame{junk};
        (void)parse_event(frame);
        EXPECT_FALSE(parse_book_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_trades(frame, *trades).has_value()) << junk;
        EXPECT_FALSE(parse_ticker(frame).has_value()) << junk;
        EXPECT_FALSE(parse_book_update(frame, *levels).has_value()) << junk;
        EXPECT_FALSE(parse_server_time(frame).has_value()) << junk;
        EXPECT_FALSE(parse_rest_error(frame).has_value()) << junk;
        if (frame != "[]") {
            EXPECT_FALSE(parse_contracts(frame, *contracts).has_value()) << junk;
        }
    }
}

// -------------------------------------------------------------- map ----

// BTC_USDT as Gate publishes it: 0.1 tick, one contract a lot, 0.0001 BTC a
// contract.
exec::InstrumentTable table_with_btc() {
    exec::InstrumentTable table;
    table.reserve(1);
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.1";
    wire.order_size_min = "1";
    wire.quanto_multiplier = "0.0001";
    wire.funding_interval_s = 28'800;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

constexpr UnixNanos k_ingest{1'700'000'000'000'000'000ULL};

TEST(GateMap, BookTickerBecomesTopOfBook) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.contract = "BTC_USDT";
    wire.bid_price = "64155.8";
    wire.bid_qty = "76800";
    wire.ask_price = "64155.9";
    wire.ask_qty = "535";
    wire.ts_ms = 1'786'394'633'457ULL;

    const auto bbo = map_bbo(wire, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 641'558); // 64155.8 / 0.1
    EXPECT_EQ(bbo->ask.raw, 641'559);
    // Contracts, one per lot: 76800 contracts is 7.68 BTC at 0.0001 each.
    EXPECT_EQ(bbo->bid_qty.raw, 76'800);
    EXPECT_EQ(bbo->ask_qty.raw, 535);
    EXPECT_EQ(bbo->ts_event.ns, 1'786'394'633'457ULL * 1'000'000ULL);
    EXPECT_EQ(bbo->ts_init.ns, k_ingest.ns);
}

TEST(GateMap, PriceOffTheTickIsDroppedNotRounded) {
    // A price that does not divide by the tick means Gate changed an
    // instrument's precision under us. Rounding would put an order one tick
    // from where it was meant.
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.contract = "BTC_USDT";
    wire.bid_price = "64155.85"; // 0.05 past a 0.1 tick
    wire.bid_qty = "1";
    wire.ask_price = "64155.9";
    wire.ask_qty = "1";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(GateMap, FractionalContractCountIsDroppedNotRounded) {
    // Public market data quotes integer contracts today. If Gate ever sends a
    // decimal size on an enable_decimal contract, that one message is dropped
    // rather than silently truncated to a different quantity.
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.contract = "BTC_USDT";
    wire.bid_price = "64155.8";
    wire.bid_qty = "1.5";
    wire.ask_price = "64155.9";
    wire.ask_qty = "1";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(GateMap, UnknownInstrumentIsDropped) {
    const auto table = table_with_btc();
    WireBookTicker wire{};
    wire.contract = "DOGE_USDT";
    wire.bid_price = "0.1";
    wire.bid_qty = "1";
    wire.ask_price = "0.2";
    wire.ask_qty = "1";
    EXPECT_FALSE(map_bbo(wire, table, k_ingest).has_value());
}

TEST(GateMap, AggressorSideSurvivesMapping) {
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.contract = "BTC_USDT";
    wire.price = "64161.3";
    wire.qty = "2184";
    wire.ts_ms = 1'786'394'635'667ULL;

    wire.aggressor_bought = false;
    auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Sell);

    wire.aggressor_bought = true;
    trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->aggressor, Side::Buy);
    EXPECT_EQ(trade->price.raw, 641'613);
    EXPECT_EQ(trade->qty.raw, 2'184);
}

TEST(GateMap, MissingVenueTimeFallsBackToIngest) {
    const auto table = table_with_btc();
    WireTrade wire{};
    wire.contract = "BTC_USDT";
    wire.price = "64161.3";
    wire.qty = "12";
    wire.ts_ms = 0;
    const auto trade = map_trade(wire, table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    // Not 1970: a zero timestamp reaching a latency histogram poisons it.
    EXPECT_EQ(trade->ts_event.ns, k_ingest.ns);
}

TEST(GateMap, FundingSettlementIsSecondsAndTheTickIsMilliseconds) {
    // The whole point of the two converters. funding_next_apply is seconds and
    // `t` on the same object is milliseconds; swapping them is silent and puts
    // the settlement either in 1970 or 30 millennia out.
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.contract = "BTC_USDT";
    wire.funding_rate = "0.000064";
    wire.mark_price = "64163.2";
    wire.funding_interval_s = 28'800;
    wire.funding_next_apply_s = 1'786'406'400ULL;
    wire.ts_ms = 1'786'394'640'101ULL;

    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 64'000); // 0.000064 * 1e9
    EXPECT_EQ(funding->next_funding_time.ns, 1'786'406'400ULL * 1'000'000'000ULL);
    EXPECT_EQ(funding->ts_event.ns, 1'786'394'640'101ULL * 1'000'000ULL);
    // Stated by the venue on this very frame, so nothing has to be derived
    // from the gap between two settlement times the way okx forces.
    EXPECT_EQ(funding->interval_s, 28'800U);
    // Mark rides the same channel here, unlike okx where it has its own.
    EXPECT_EQ(funding->mark_price.raw, 641'632);
}

TEST(GateMap, IndicativeRateIsNotPublishedAsAForecast) {
    // Gate's SDK marks funding_rate_indicative deprecated in favour of
    // funding_rate, and it was equal to it on every observed frame. Mapping it
    // would dress a copy of the current rate up as a next-interval estimate.
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.contract = "BTC_USDT";
    wire.funding_rate = "0.000064";
    wire.funding_rate_indicative = "0.000064";
    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_TRUE(funding->predicted_rate.is_undef());
}

TEST(GateMap, MarkOffTheOrderGridDoesNotCostTheFundingRate) {
    // Gate quotes mark on mark_price_round, a finer grid than
    // order_price_round on some contracts, so a mark that will not divide is
    // normal rather than a schema event — and funding is what this update is
    // for.
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.contract = "BTC_USDT";
    wire.funding_rate = "0.000064";
    wire.mark_price = "64163.25"; // 0.05 past a 0.1 order tick
    const auto funding = map_funding(wire, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    EXPECT_EQ(funding->rate.raw, 64'000);
    EXPECT_TRUE(funding->mark_price.is_undef());
}

TEST(GateMap, ContractWithoutFundingIsNotReportedAsZero) {
    const auto table = table_with_btc();
    WireTicker wire{};
    wire.contract = "BTC_USDT";
    wire.funding_rate = {}; // absent, not "0"
    EXPECT_FALSE(map_funding(wire, table, k_ingest).has_value());
}

TEST(GateMap, ContractValueComesFromQuantoMultiplier) {
    // A Gate perp quotes in contracts. Pinning contract_value to 1 the way
    // binance USD-M allows would misprice every BTC_USDT notional by 10000x.
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.1";
    wire.quanto_multiplier = "0.0001";
    auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 100'000); // 0.0001 * 1e9
    // One whole contract per lot: Gate publishes no size step, and public
    // market data quotes integer contract counts on every contract.
    EXPECT_EQ(base_of(*instrument).lot_size.raw, k_e9);

    // A meme contract is worth ten million base units, and the same field
    // carries it.
    wire.name = "PEPE_USDT";
    wire.quanto_multiplier = "10000000";
    instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    EXPECT_EQ(base_of(*instrument).contract_value.raw, 10'000'000LL * k_e9);
}

TEST(GateMap, FundingIntervalIsCarriedOntoThePerp) {
    // 4 h contracts outnumber 8 h ones on 341 of 899 rows, so an assumed
    // interval would be wrong for a third of the venue and invisible when it
    // is.
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.1";
    wire.quanto_multiplier = "0.0001";
    wire.funding_interval_s = 14'400;
    const auto instrument = map_instrument(wire, MarketType::Perp);
    ASSERT_TRUE(instrument.has_value());
    ASSERT_TRUE(std::holds_alternative<PerpInstrument>(*instrument));
    EXPECT_EQ(std::get<PerpInstrument>(*instrument).funding_interval_s, 14'400U);
}

TEST(GateMap, DelistingAndNonTradingRowsAreRefused) {
    WireContract wire{};
    wire.name = "DOGE_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.00001";
    wire.quanto_multiplier = "10";
    ASSERT_TRUE(map_instrument(wire, MarketType::Perp).has_value());

    // in_delisting stays true through the reduce-only window while every other
    // field still looks tradable.
    wire.in_delisting = true;
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.in_delisting = false;
    for (const char* status : {"prelaunch", "delisting", "delisted", "circuit_breaker", ""}) {
        wire.status = status;
        EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value()) << status;
    }
}

TEST(GateMap, SpotAndDatedMarketsAreRefused) {
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0.1";
    wire.quanto_multiplier = "0.0001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Spot).has_value());
    EXPECT_FALSE(map_instrument(wire, MarketType::Future).has_value());
}

TEST(GateMap, InstrumentWithoutUsableStepsIsRefused) {
    // Every later conversion divides by these, so a zero step must never reach
    // the table.
    WireContract wire{};
    wire.name = "BTC_USDT";
    wire.status = "trading";
    wire.order_price_round = "0";
    wire.quanto_multiplier = "0.0001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    // KNOWN LIMIT: FixedE9 carries nine fractional digits and Gate lists three
    // contracts at ticks down to 1e-11. Refusing beats rounding.
    wire.order_price_round = "0.00000000001";
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());

    wire.order_price_round = "0.1";
    wire.quanto_multiplier = {};
    EXPECT_FALSE(map_instrument(wire, MarketType::Perp).has_value());
}

TEST(GateMap, LiveCaptureFlowsFromFrameToDomain) {
    // End to end on real bytes: the catalogue builds the table and the three
    // consumed channels map against it, so a unit change anywhere in the chain
    // shows up here rather than as a quiet gateway.
    auto storage = std::make_unique<ContractBuffer>();
    const auto body = fixture("rest_contracts.json");
    const auto rows = parse_contracts(body, *storage);
    ASSERT_TRUE(rows.has_value());
    exec::InstrumentTable table;
    // Before any add(): the table's keys are views into its own storage, so a
    // reallocation mid-load dangles every symbol added so far.
    table.reserve(rows->size());
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Perp); instrument.has_value()) {
            EXPECT_TRUE(table.add(*instrument).has_value());
        }
    }
    // BTC and ETH load; the delisting row and the 1e-11-tick row do not.
    EXPECT_EQ(table.size(), 2U);

    const auto book_frame = fixture("ws_book_ticker.json");
    const auto book = parse_book_ticker(book_frame);
    ASSERT_TRUE(book.has_value());
    const auto bbo = map_bbo(*book, table, k_ingest);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_LT(bbo->bid.raw, bbo->ask.raw);
    EXPECT_GT(bbo->bid_qty.raw, 0);

    auto trades_storage = std::make_unique<TradeBuffer>();
    const auto trade_frame = fixture("ws_trades.json");
    const auto trades = parse_trades(trade_frame, *trades_storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_FALSE(trades->empty());
    const auto trade = map_trade(trades->front(), table, k_ingest);
    ASSERT_TRUE(trade.has_value());
    EXPECT_GT(trade->qty.raw, 0);

    const auto ticker_frame = fixture("ws_tickers.json");
    const auto ticker = parse_ticker(ticker_frame);
    ASSERT_TRUE(ticker.has_value());
    const auto funding = map_funding(*ticker, table, k_ingest);
    ASSERT_TRUE(funding.has_value());
    // Seconds in, nanoseconds out, and still inside the window a real
    // settlement lives in.
    EXPECT_GT(funding->next_funding_time.ns, 1'700'000'000'000'000'000ULL);
    EXPECT_LT(funding->next_funding_time.ns, 4'000'000'000'000'000'000ULL);
}

// --------------------------------------------------------------- ws ----

TEST(GateWs, SubscribeFrameNamesOneChannelAndListsContracts) {
    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.channel = "futures.book_ticker", .contract = "BTC_USDT"},
        Subscription{.channel = "futures.book_ticker", .contract = "ETH_USDT"}};
    const auto frame = protocol.subscribe_frame(subs, 1'786'394'633ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);
    EXPECT_EQ(frame->text,
              R"({"time":1786394633,"channel":"futures.book_ticker","event":"subscribe",)"
              R"("payload":["BTC_USDT","ETH_USDT"]})");
}

TEST(GateWs, BatchingStopsAtAChannelBoundary) {
    // One frame names one channel, so a mixed span has to be drained in
    // several calls or the second channel would be silently dropped.
    Protocol protocol;
    const std::array<Subscription, 3> subs{
        Subscription{.channel = "futures.book_ticker", .contract = "BTC_USDT"},
        Subscription{.channel = "futures.book_ticker", .contract = "ETH_USDT"},
        Subscription{.channel = "futures.trades", .contract = "BTC_USDT"}};
    auto frame = protocol.subscribe_frame(subs, 1ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 2U);

    frame = protocol.subscribe_frame(std::span<const Subscription>{subs}.subspan(2), 1ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 1U);
    EXPECT_NE(frame->text.find("futures.trades"), std::string_view::npos);
}

TEST(GateWs, DepthChannelSpendsItsPayloadOnIntervalAndLevel) {
    // [contract, interval, level] leaves no room for a second contract, so it
    // is one per frame however many are queued.
    Protocol protocol;
    const std::array<Subscription, 2> subs{
        Subscription{.channel = "futures.order_book_update", .contract = "BTC_USDT"},
        Subscription{.channel = "futures.order_book_update", .contract = "ETH_USDT"}};
    const auto frame = protocol.subscribe_frame(subs, 7ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->consumed, 1U);
    EXPECT_EQ(frame->text,
              R"({"time":7,"channel":"futures.order_book_update","event":"subscribe",)"
              R"("payload":["BTC_USDT","100ms","20"]})");
}

TEST(GateWs, FundingAndMarkPriceShareOneChannel) {
    EXPECT_EQ(channel_for(exec::SubKind::Funding), channel_for(exec::SubKind::MarkPrice));
    EXPECT_EQ(channel_for(exec::SubKind::Funding), "futures.tickers");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline).empty());
}

TEST(GateWs, EmptyChannelIsRefusedRatherThanSent) {
    // channel_for() declined the kind; sending it would subscribe to the
    // literal "" and earn a code-2 refusal.
    Protocol protocol;
    const std::array<Subscription, 1> subs{Subscription{.channel = {}, .contract = "BTC_USDT"}};
    const auto frame = protocol.subscribe_frame(subs, 1ULL);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::UnsupportedKind);
    EXPECT_FALSE(protocol.subscribe_frame({}, 1ULL).has_value());
}

TEST(GateWs, PingFrameCarriesATimestamp) {
    // Not a constant like okx's four-byte "ping": Gate wants a JSON frame on
    // its own channel, and answers on futures.pong.
    Protocol protocol;
    EXPECT_EQ(protocol.ping_frame(1'786'394'751ULL),
              R"({"time":1786394751,"channel":"futures.ping"})");
}

TEST(GateWs, UnsubscribeMirrorsSubscribe) {
    Protocol protocol;
    const std::array<Subscription, 1> subs{
        Subscription{.channel = "futures.trades", .contract = "BTC_USDT"}};
    const auto frame = protocol.unsubscribe_frame(subs, 5ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(frame->text.find(R"("event":"unsubscribe")"), std::string_view::npos);
}

// ------------------------------------------------------------- book ----
//
// The three seed fixtures are one capture: `rest_order_book_seed.json` is the
// snapshot, `ws_order_book_straddle.json` is the frame whose [U, u] contains
// its id + 1, and `ws_order_book_straddle_next.json` is the frame after it. A
// seed and a stream from different minutes replay into a crossed book, which
// is what these tests exist to catch.

// The parsed views point into the frame text and into the level buffer, so
// both have to outlive the wire struct that came out of them.
struct BookFrame {
    std::string text;
    std::unique_ptr<LevelBuffer> storage{std::make_unique<LevelBuffer>()};
};

DepthBook book_for(const exec::InstrumentTable& table) {
    const auto* instrument = table.find("BTC_USDT");
    EXPECT_NE(instrument, nullptr);
    const auto& base = base_of(*instrument);
    return DepthBook{base.id, base.tick_size, base.lot_size};
}

WireBookUpdate delta_of(BookFrame& frame, std::string_view name) {
    frame.text = fixture(name);
    const auto wire = parse_book_update(frame.text, *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireBookUpdate{});
}

WireOrderBook seed_of(BookFrame& frame) {
    frame.text = fixture("rest_order_book_seed.json");
    const auto wire = parse_order_book(frame.text, *frame.storage);
    EXPECT_TRUE(wire.has_value());
    return wire.value_or(WireOrderBook{});
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

TEST(GateBook, AnUnseededBookPublishesNothing) {
    // There is no snapshot on this channel at all, so an adapter that trusted
    // the first delta would be quoting a book made of one frame's worth of
    // changes.
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame first;
    EXPECT_TRUE(book.wants_seed());
    EXPECT_EQ(book.apply(delta_of(first, "ws_order_book_straddle.json")), BookResult::Buffered);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
}

TEST(GateBook, SeedAndStreamMeetInABookSharedMemoryCanTake) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame seed_frame;
    BookFrame first;
    BookFrame second;
    const auto snapshot = seed_of(seed_frame);
    EXPECT_EQ(book.seed(snapshot), BookResult::Waiting);
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());

    const auto d1 = delta_of(first, "ws_order_book_straddle.json");
    // The chain is contiguous, so the frame that continues the seed is the one
    // holding id + 1 — not the one holding id, which is already in the levels.
    ASSERT_LE(d1.first_update_id, snapshot.update_id + 1);
    ASSERT_GE(d1.last_update_id, snapshot.update_id + 1);
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    const auto d2 = delta_of(second, "ws_order_book_straddle_next.json");
    EXPECT_EQ(d2.first_update_id, d1.last_update_id + 1);
    // A third of the frames carry no levels at all. They still advance the
    // chain, so refusing them would break every later contiguity check.
    EXPECT_TRUE(d2.bids.empty());
    EXPECT_TRUE(d2.asks.empty());
    EXPECT_EQ(book.apply(d2), BookResult::Applied);

    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // Gate maintains a fixed subscribed window, so the book holds exactly what
    // the venue keeps — fewer than the snapshot could carry, and that is the
    // point: nothing is invented to fill the array.
    EXPECT_EQ(out->bid_count, k_gate_window);
    EXPECT_EQ(out->ask_count, k_gate_window);
    static_assert(k_gate_window <= k_book_levels);
    EXPECT_EQ(out->venue_seq, d2.last_update_id) << "u is the forensic handle";
    // Gate maintains the subscribed window itself, deleting what falls out of
    // it, so the book neither grows nor erodes.
    EXPECT_EQ(book.bids().size(), k_depth_limit);
    EXPECT_EQ(book.asks().size(), k_depth_limit);
}

TEST(GateBook, DeltasBufferedBehindTheSeedReplayIntoTheSameBook) {
    // Which of the REST answer and the websocket frame lands first is a race,
    // and the book it produces must not depend on who won.
    const auto table = table_with_btc();
    BookFrame seed_frame;
    BookFrame first;
    BookFrame second;
    const auto snapshot = seed_of(seed_frame);
    const auto d1 = delta_of(first, "ws_order_book_straddle.json");
    const auto d2 = delta_of(second, "ws_order_book_straddle_next.json");

    auto streamed = book_for(table);
    EXPECT_EQ(streamed.seed(snapshot), BookResult::Waiting);
    EXPECT_EQ(streamed.apply(d1), BookResult::Applied);
    EXPECT_EQ(streamed.apply(d2), BookResult::Applied);

    auto buffered = book_for(table);
    EXPECT_EQ(buffered.apply(d1), BookResult::Buffered);
    EXPECT_EQ(buffered.apply(d2), BookResult::Buffered);
    EXPECT_EQ(buffered.seed(snapshot), BookResult::Applied);

    const auto from_stream = streamed.snapshot(k_ingest, k_ingest);
    const auto from_buffer = buffered.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(from_stream.has_value());
    ASSERT_TRUE(from_buffer.has_value());
    expect_coherent(*from_buffer);
    EXPECT_EQ(from_buffer->venue_seq, from_stream->venue_seq);
    for (std::uint32_t i = 0; i < k_book_levels; ++i) {
        EXPECT_EQ(from_buffer->bids[i].price.raw, from_stream->bids[i].price.raw) << i;
        EXPECT_EQ(from_buffer->bids[i].qty.raw, from_stream->bids[i].qty.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].price.raw, from_stream->asks[i].price.raw) << i;
        EXPECT_EQ(from_buffer->asks[i].qty.raw, from_stream->asks[i].qty.raw) << i;
    }
}

TEST(GateBook, AGapInTheChainPublishesNothing) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame seed_frame;
    BookFrame first;
    BookFrame second;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    EXPECT_EQ(book.apply(delta_of(first, "ws_order_book_straddle.json")), BookResult::Applied);

    auto skipped = delta_of(second, "ws_order_book_straddle_next.json");
    skipped.first_update_id += 1; // one id of the chain went missing

    EXPECT_EQ(book.apply(skipped), BookResult::Gap);
    EXPECT_EQ(book.gaps(), 1U);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_TRUE(book.wants_seed()) << "a dropped book asks for a new seed";
}

TEST(GateBook, ZeroSizeDeletesTheLevel) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame seed_frame;
    BookFrame first;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    const auto d1 = delta_of(first, "ws_order_book_straddle.json");
    EXPECT_EQ(book.apply(d1), BookResult::Applied);

    const auto before = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->asks[0].price.raw, 631'327) << "63132.7 over a 0.1 tick";
    const auto next_up = before->asks[1].price.raw;

    const std::array<WireLevel, 1> gone{WireLevel{.price = "63132.7", .qty = "0"}};
    WireBookUpdate lifted{};
    lifted.contract = "BTC_USDT";
    lifted.first_update_id = d1.last_update_id + 1;
    lifted.last_update_id = d1.last_update_id + 2;
    lifted.asks = gone;
    EXPECT_EQ(book.apply(lifted), BookResult::Applied);

    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->asks[0].price.raw, next_up) << "a zero deletes, it does not rest at zero";
    EXPECT_EQ(after->asks[0].qty.raw, before->asks[1].qty.raw);
}

TEST(GateBook, ALevelOffTheTickIsDroppedNotRounded) {
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame seed_frame;
    BookFrame first;
    EXPECT_EQ(book.seed(seed_of(seed_frame)), BookResult::Waiting);
    const auto d1 = delta_of(first, "ws_order_book_straddle.json");
    EXPECT_EQ(book.apply(d1), BookResult::Applied);
    const auto dropped_before = book.dropped_levels();
    const auto before = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(before.has_value());

    // 0.05 past a 0.1 tick, and a fractional contract count where sizes are
    // whole contracts. Either one alone is enough to refuse the level.
    const std::array<WireLevel, 2> bad_levels{WireLevel{.price = "63132.65", .qty = "100"},
                                              WireLevel{.price = "63132.6", .qty = "0.5"}};
    WireBookUpdate bad{};
    bad.contract = "BTC_USDT";
    bad.first_update_id = d1.last_update_id + 1;
    bad.last_update_id = d1.last_update_id + 2;
    bad.bids = bad_levels;
    EXPECT_EQ(book.apply(bad), BookResult::Applied);

    EXPECT_EQ(book.dropped_levels(), dropped_before + 2);
    const auto after = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(after.has_value());
    for (std::uint32_t i = 0; i < k_book_levels; ++i) {
        EXPECT_EQ(after->bids[i].price.raw, before->bids[i].price.raw) << i;
        EXPECT_EQ(after->bids[i].qty.raw, before->bids[i].qty.raw) << i;
    }
}

TEST(GateBook, ASeedTheStreamHasRunPastIsDiscarded) {
    // The chain is contiguous, so a seed the stream has already overtaken can
    // never be reached — the ids in between are gone. Splicing it in anyway
    // would produce a book missing every change in that window.
    const auto table = table_with_btc();
    auto book = book_for(table);
    BookFrame seed_frame;
    BookFrame first;
    auto snapshot = seed_of(seed_frame);
    const auto d1 = delta_of(first, "ws_order_book_straddle.json");
    snapshot.update_id = d1.first_update_id - 2;

    EXPECT_EQ(book.seed(snapshot), BookResult::Waiting);
    EXPECT_EQ(book.apply(d1), BookResult::Reseed);
    EXPECT_FALSE(book.live());
    EXPECT_FALSE(book.snapshot(k_ingest, k_ingest).has_value());
    EXPECT_TRUE(book.wants_seed());
}

// ------------------------------------------------------------- rest ----

TEST(GateRest, TargetsCarryTheSettleCurrency) {
    RequestBuilder builder;
    EXPECT_EQ(builder.contracts(k_settle_usdt).target, "/api/v4/futures/usdt/contracts");
    // with_id is not optional here: without it the body has no `id` and the
    // levels cannot be placed in the update chain.
    EXPECT_EQ(builder.order_book(k_settle_usdt, "BTC_USDT", 5).target,
              "/api/v4/futures/usdt/order_book?contract=BTC_USDT&limit=5&with_id=true");
    // The only clock endpoint Gate publishes lives under spot, not futures.
    EXPECT_EQ(builder.server_time().target, "/api/v4/spot/time");
}

TEST(GateRest, TestnetIsADifferentHost) {
    EXPECT_EQ(rest_host(false), "api.gateio.ws");
    EXPECT_NE(rest_host(true), rest_host(false));
    EXPECT_NE(stream_host(true), stream_host(false));
}

// ----------------------------------------------------------- errors ----

TEST(GateErrors, AuthLabelsHaltTheVenue) {
    for (const auto label : {k_invalid_key,
                             k_invalid_signature,
                             k_authentication_failed,
                             k_ip_forbidden,
                             k_read_only,
                             k_missing_required_header}) {
        EXPECT_EQ(classify(label), exec::ErrorClass::AuthFailure) << label;
    }
}

TEST(GateErrors, ClockSkewIsRetryableNotAuthFailure) {
    // The request was fine, our clock was not. Resync and resend succeeds, and
    // filing this under AuthFailure — which is where Gate's own docs and ccxt
    // put it — would halt a venue whose keys are valid.
    EXPECT_EQ(classify(k_request_expired), exec::ErrorClass::Retryable);
}

TEST(GateErrors, RateLimitLabelsBackOff) {
    EXPECT_EQ(classify(k_too_many_requests), exec::ErrorClass::RateLimited);
    EXPECT_EQ(classify(k_too_fast), exec::ErrorClass::RateLimited);
}

TEST(GateErrors, ServerSideLabelsRetry) {
    for (const auto label : {k_server_error, k_too_busy, k_internal}) {
        EXPECT_EQ(classify(label), exec::ErrorClass::Retryable) << label;
    }
}

TEST(GateErrors, UnknownLabelIsTerminalByDefault) {
    // Deliberate: an unknown refusal retried at full speed earns an IP ban,
    // while a spuriously terminal order is recovered by reconciliation.
    EXPECT_EQ(classify("SOME_LABEL_GATE_ADDED_LAST_TUESDAY"), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify({}), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_contract_not_found), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_too_big), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_poc_fill_immediately), exec::ErrorClass::Terminal);
}

TEST(GateErrors, StreamCodesAreNumbersAndClassifySeparately) {
    // The websocket speaks integers where REST speaks labels; one table cannot
    // serve both.
    EXPECT_EQ(classify_stream(k_ws_invalid_argument), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify_stream(k_ws_auth_failed), exec::ErrorClass::AuthFailure);
    EXPECT_EQ(classify_stream(k_ws_invalid_key), exec::ErrorClass::AuthFailure);
    // A lapsed session is not bad credentials: logging in again is the fix.
    EXPECT_EQ(classify_stream(k_ws_not_logged_in), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_stream(k_ws_server_error), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify_stream(999), exec::ErrorClass::Terminal);
}

TEST(GateErrors, BenignCancelRacesAreRecognised) {
    EXPECT_TRUE(is_benign_cancel_failure(k_order_not_found));
    EXPECT_TRUE(is_benign_cancel_failure(k_order_finished));
    EXPECT_TRUE(is_benign_cancel_failure(k_order_cancelled));
    EXPECT_FALSE(is_benign_cancel_failure(k_balance_not_enough));
}

} // namespace
} // namespace raikiri::venues::gate
