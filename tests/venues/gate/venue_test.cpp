#include <raikiri/net/signer.hpp>
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
constexpr std::uint32_t k_gate_window = 100;

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

TEST(GateVenue, AnUnbuildableCommandIsRefusedLocally) {
    // No credentials, no instrument, no order id: nothing about this command
    // can reach the wire. Silence would leave it open in the store forever.
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

TEST(GateVenue, MarketsWithNoCatalogueLoaderAreRefusedRatherThanReturnedEmpty) {
    // An empty catalogue reads as "this venue lists nothing", which is a very
    // different claim from "this adapter does not load that market". Both
    // refusals happen before any socket opens, so this test touches no network.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Gate, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    for (const auto market : {MarketType::Option, MarketType::Future}) {
        const auto loaded = adapter->instruments->fetch_all(market);
        ASSERT_FALSE(loaded.has_value()) << to_string(market);
        EXPECT_EQ(loaded.error().cls, exec::ErrorClass::Terminal);
    }
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
    const auto sells = parse_trades(sell_frame, *storage, MarketType::Perp);
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
    const auto buys = parse_trades(buy_frame, *buy_storage, MarketType::Perp);
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
    EXPECT_EQ(seed->update_id, 121'737'510'350ULL);
    EXPECT_EQ(seed->update_s, 1'786'984'119ULL) << "fractional seconds, truncated";
    EXPECT_EQ(seed->bids.size(), k_gate_window);
    EXPECT_EQ(seed->asks.size(), k_gate_window);

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
        EXPECT_FALSE(parse_trades(frame, *trades, MarketType::Perp).has_value()) << junk;
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
    const auto trades = parse_trades(trade_frame, *trades_storage, MarketType::Perp);
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
              R"("payload":["BTC_USDT","100ms","100"]})");
}

TEST(GateWs, FundingAndMarkPriceShareOneChannel) {
    EXPECT_EQ(channel_for(exec::SubKind::Funding, MarketType::Perp), channel_for(exec::SubKind::MarkPrice, MarketType::Perp));
    EXPECT_EQ(channel_for(exec::SubKind::Funding, MarketType::Perp), "futures.tickers");
    EXPECT_TRUE(channel_for(exec::SubKind::Kline, MarketType::Perp).empty());
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
    // A tenth of the frames carry no levels at all. They still advance the
    // chain, so refusing them would break every later contiguity check.
    EXPECT_TRUE(d2.bids.empty());
    EXPECT_TRUE(d2.asks.empty());
    EXPECT_EQ(book.apply(d2), BookResult::Applied);

    const auto out = book.snapshot(k_ingest, k_ingest);
    ASSERT_TRUE(out.has_value());
    expect_coherent(*out);
    // Gate maintains a fixed subscribed window, so the book holds exactly what
    // the venue keeps — every subscribed level and not one invented to fill
    // the array.
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
    ASSERT_EQ(before->asks[0].price.raw, 640'616) << "64061.6 over a 0.1 tick";
    const auto next_up = before->asks[1].price.raw;

    const std::array<WireLevel, 1> gone{WireLevel{.price = "64061.6", .qty = "0"}};
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
    const std::array<WireLevel, 2> bad_levels{WireLevel{.price = "64061.55", .qty = "100"},
                                              WireLevel{.price = "64061.5", .qty = "0.5"}};
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


// ---------------------------------------------------------- signing ----

// Gate's own published worked examples. They pin the two things that make
// this signature unlike every other venue's: SHA512 rather than SHA256, and a
// body that appears only as its own hash.
constexpr std::string_view k_doc_key = "key";
constexpr std::string_view k_doc_secret = "secret";
constexpr std::string_view k_empty_sha512 =
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";

TEST(GateSign, AnEmptyBodyIsHashedRatherThanSkipped) {
    // The term is always present and always 128 characters: a GET that leaves
    // it blank signs a different string than the venue does.
    std::array<char, net::k_hex512_bytes> out{};
    EXPECT_EQ(net::sha512_hex({}, out), k_empty_sha512);
}

TEST(GateSign, PublishedVectorsReproduce) {
    std::array<char, net::k_hex512_bytes> out{};

    const std::string get_message =
        std::string{"GET\n/api/v4/futures/orders\ncontract=BTC_USD&status=finished&limit=50\n"} +
        std::string{k_empty_sha512} + "\n1541993715";
    EXPECT_EQ(net::hmac_sha512_hex(k_doc_secret, get_message, out),
              "55f84ea195d6fe57ce62464daaa7c3c02fa9d1dde954e4c898289c9a2407a3d6"
              "fb3faf24deff16790d726b66ac9f74526668b13bd01029199cc4fcc522418b8a");

    constexpr std::string_view post_body =
        R"({"contract":"BTC_USD","type":"limit","size":100,"price":6800,"time_in_force":"gtc"})";
    std::array<char, net::k_hex512_bytes> body_hash{};
    EXPECT_EQ(net::sha512_hex(post_body, body_hash),
              "ad3c169203dc3026558f01b4df307641fa1fa361f086b2306658886d5708767b"
              "1854797c68d9e62fef2f991645aa82673622ebf417e091d0bd22bafe5d956cca");

    const std::string post_message = std::string{"POST\n/api/v4/futures/orders\n\n"} +
                                     std::string{net::sha512_hex(post_body, body_hash)} +
                                     "\n1541993715";
    EXPECT_EQ(net::hmac_sha512_hex(k_doc_secret, post_message, out),
              "eae42da914a590ddf727473aff25fc87d50b64783941061f47a3fdb92742541f"
              "c4c2c14017581b4199a1418d54471c269c03a38d788d802e2c306c37636389f0");
}

TEST(GateSign, ClientOrderIdFitsBehindTheRequiredPrefix) {
    // 26 Crockford characters plus "t-" is 28 bytes, and Gate's limit is 28
    // bytes AFTER the prefix. If a future id scheme grew, the static_assert in
    // rest.hpp would fail the build rather than the venue failing the order.
    const auto ulid = ExchangeClientOrderId::from("01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    ASSERT_TRUE(ulid.has_value());
    std::array<char, k_client_id_bytes> buf{};
    const auto text = client_id_text(*ulid, buf);
    EXPECT_EQ(text, "t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    EXPECT_EQ(text.size(), 28U);
    EXPECT_EQ(client_id_from_text(text), ulid->view());
}

TEST(GateSign, GatesOwnOrderSourcesAreNotMistakenForOurs) {
    // `api`, `web` and `liq-...` are Gate's reserved values for orders we did
    // not place. Reading one as a client id attributes a liquidation to a
    // strategy's order.
    EXPECT_TRUE(client_id_from_text("api").empty());
    EXPECT_TRUE(client_id_from_text("web").empty());
    EXPECT_TRUE(client_id_from_text("liq-1234").empty());
    EXPECT_TRUE(client_id_from_text({}).empty());
}

// ---------------------------------------------------- signed requests ----

constexpr UnixNanos k_sign_now{1'541'993'715'000'000'000ULL};

ExchangeClientOrderId sample_coid() {
    const auto id = ExchangeClientOrderId::from("01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    EXPECT_TRUE(id.has_value());
    return id.value_or(ExchangeClientOrderId{});
}

// BTC_USDT as Gate lists it on spot: price to one decimal, quantity to six.
// Taken from the live `rest_currency_pairs.json` row, not invented.
exec::InstrumentTable spot_table_with_btc() {
    exec::InstrumentTable table;
    CurrencyPairBuffer storage;
    const auto body = fixture("rest_currency_pairs.json");
    const auto wire = parse_currency_pairs(body, storage);
    EXPECT_TRUE(wire.has_value());
    for (const auto& row : *wire) {
        if (row.id != "BTC_USDT") {
            continue;
        }
        const auto instrument = map_currency_pair(row);
        EXPECT_TRUE(instrument.has_value());
        EXPECT_TRUE(table.add(*instrument).has_value());
    }
    return table;
}

OrderCmd limit_cmd(MarketType market, Side side, PriceTicks price, QtyLots qty) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.instrument = InstrumentId::make(VenueId::Gate, market, SymbolId{0});
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.exchange_client_order_id = sample_coid();
    cmd.price = price;
    cmd.qty = qty;
    return cmd;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_doc_key);
    ASSERT_TRUE(signer.secret().assign(k_doc_secret).has_value());
}

std::string_view header_of(const net::RestRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

TEST(GateSignedRest, SignatureLayoutIsMethodPathQueryBodyHashSeconds) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = table_with_btc();

    const auto request = builder.create_order(
        limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                  QtyLots::from_raw(10)),
        table,
        k_sign_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    std::array<char, net::k_hex512_bytes> body_hash{};
    const auto expected =
        std::string{"POST\n/api/v4/futures/usdt/orders\n\n"} +
        std::string{net::sha512_hex(request->body, body_hash)} + "\n1541993715";
    EXPECT_EQ(builder.last_sign_message(), expected);

    // The Timestamp header and the digest's last line must be the same value;
    // Gate compares the header against its own clock and the digest against
    // the header.
    EXPECT_EQ(header_of(*request, "Timestamp"), "1541993715");
    EXPECT_EQ(header_of(*request, "KEY"), k_doc_key);
    std::array<char, net::k_hex512_bytes> signature{};
    EXPECT_EQ(header_of(*request, "SIGN"),
              net::hmac_sha512_hex(k_doc_secret, builder.last_sign_message(), signature));
}

TEST(GateSignedRest, WithoutCredentialsNothingIsBuilt) {
    net::Signer signer; // no key, no secret
    SignedRequestBuilder builder{signer};
    const auto table = table_with_btc();
    const auto request = builder.create_order(
        limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                  QtyLots::from_raw(1)),
        table,
        k_sign_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::NotSigned);
}

TEST(GateSignedRest, PerpOrderCarriesTheSideInTheSignOfSize) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = table_with_btc();

    auto cmd = limit_cmd(MarketType::Perp, Side::Sell, PriceTicks::from_raw(640'000),
                         QtyLots::from_raw(10));
    cmd.flags = OrderFlags::PostOnly;
    const auto sell = builder.create_order(cmd, table, k_sign_now);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->target, "/api/v4/futures/usdt/orders");
    // Post-only is a tif on Gate, not a flag beside one.
    EXPECT_EQ(sell->body,
              R"({"contract":"BTC_USDT","size":-10,"price":"64000","tif":"poc",)"
              R"("text":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ"})");

    const auto buy = builder.create_order(
        limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                  QtyLots::from_raw(10)),
        table,
        k_sign_now);
    ASSERT_TRUE(buy.has_value());
    EXPECT_NE(buy->body.find(R"("size":10)"), std::string_view::npos) << buy->body;
    EXPECT_NE(buy->body.find(R"("tif":"gtc")"), std::string_view::npos) << buy->body;
}

TEST(GateSignedRest, PerpMarketOrderIsPriceZeroWithIoc) {
    // Gate has no market order TYPE on futures; this pair of fields is it, and
    // any other tif alongside price 0 is refused by the venue.
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = table_with_btc();

    auto cmd = limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                         QtyLots::from_raw(3));
    cmd.order_type = OrderType::Market;
    cmd.tif = TimeInForce::Gtc; // deliberately wrong; the builder must override
    cmd.flags = OrderFlags::ReduceOnly;
    const auto request = builder.create_order(cmd, table, k_sign_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->body,
              R"({"contract":"BTC_USDT","size":3,"price":"0","tif":"ioc",)"
              R"("text":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ","reduce_only":true})");
}

TEST(GateSignedRest, SpotIsADifferentEndpointAndADifferentShape) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = spot_table_with_btc();

    const auto request = builder.create_order(
        limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::from_raw(640'001),
                  QtyLots::from_raw(500'000)),
        table,
        k_sign_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());
    EXPECT_EQ(request->target, "/api/v4/spot/orders");
    EXPECT_EQ(request->body,
              R"({"text":"t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ","currency_pair":"BTC_USDT",)"
              R"("type":"limit","account":"spot","side":"buy","amount":"0.5",)"
              R"("price":"64000.1","time_in_force":"gtc"})");
}

TEST(GateSignedRest, SpotMarketBuyIsRefusedRatherThanGuessed) {
    // Gate prices a spot MARKET BUY in the QUOTE currency, and an OrderCmd
    // carries base lots. Converting would mean guessing the fill price.
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = spot_table_with_btc();

    auto cmd = limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::undef(),
                         QtyLots::from_raw(500'000));
    cmd.order_type = OrderType::Market;
    const auto refused = builder.create_order(cmd, table, k_sign_now);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), BuildError::SpotMarketBuyNotExpressible);

    // The sell side is expressible: `amount` is base there.
    cmd.side = Side::Sell;
    const auto sell = builder.create_order(cmd, table, k_sign_now);
    ASSERT_TRUE(sell.has_value());
    EXPECT_NE(sell->body.find(R"("type":"market")"), std::string_view::npos) << sell->body;
    EXPECT_NE(sell->body.find(R"("time_in_force":"ioc")"), std::string_view::npos) << sell->body;
}

TEST(GateSignedRest, AFractionalContractCountIsRefusedNotRounded) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    exec::InstrumentTable table;
    {
        // A perp whose lot step is half a contract cannot render as Gate's
        // integer `size`; rounding it would send a different order.
        WireContract wire{};
        wire.name = "BTC_USDT";
        wire.status = "trading";
        wire.order_price_round = "0.1";
        wire.order_size_min = "1";
        wire.quanto_multiplier = "0.0001";
        auto instrument = map_instrument(wire, MarketType::Perp);
        ASSERT_TRUE(instrument.has_value());
        std::get<PerpInstrument>(*instrument).base.lot_size = FixedE9::from_raw(k_e9 / 2);
        ASSERT_TRUE(table.add(*instrument).has_value());
    }
    const auto request = builder.create_order(
        limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                  QtyLots::from_raw(3)),
        table,
        k_sign_now);
    ASSERT_FALSE(request.has_value());
    EXPECT_EQ(request.error(), BuildError::BadQuantity);
}

TEST(GateSignedRest, CancelPrefersTheVenueIdAndSpotAlsoNeedsThePair) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto perp = table_with_btc();
    auto cmd = limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                         QtyLots::from_raw(1));
    cmd.kind = CmdKind::Cancel;
    const auto by_client = builder.cancel_order(cmd, perp, k_sign_now);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(by_client->method, net::HttpMethod::Delete);
    EXPECT_EQ(by_client->target, "/api/v4/futures/usdt/orders/t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ");

    // Gate resolves a `t-` id only while the order is live and for 60 seconds
    // after it finishes, so the venue id wins whenever the store has one.
    cmd.target_exchange_order_id = ExchangeOrderId::from("123456789").value();
    const auto by_venue = builder.cancel_order(cmd, perp, k_sign_now);
    ASSERT_TRUE(by_venue.has_value());
    EXPECT_EQ(by_venue->target, "/api/v4/futures/usdt/orders/123456789");

    const auto spot = spot_table_with_btc();
    auto spot_cmd = limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::from_raw(640'001),
                              QtyLots::from_raw(500'000));
    spot_cmd.kind = CmdKind::Cancel;
    const auto spot_cancel = builder.cancel_order(spot_cmd, spot, k_sign_now);
    ASSERT_TRUE(spot_cancel.has_value());
    EXPECT_EQ(spot_cancel->target,
              "/api/v4/spot/orders/t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ"
              "?currency_pair=BTC_USDT&account=spot");
}

TEST(GateSignedRest, AmendIsAPutOnFuturesAndAPatchOnSpot) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto cmd = limit_cmd(MarketType::Perp, Side::Sell, PriceTicks::from_raw(650'000),
                         QtyLots::from_raw(7));
    cmd.kind = CmdKind::Amend;
    const auto perp = builder.amend_order(cmd, table_with_btc(), k_sign_now);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->method, net::HttpMethod::Put);
    // An amend re-states the SIGNED size; dropping the sign would flip a short.
    EXPECT_EQ(perp->body, R"({"size":-7,"price":"65000"})");

    auto spot_cmd = limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::from_raw(640'001),
                              QtyLots::from_raw(500'000));
    spot_cmd.kind = CmdKind::Amend;
    const auto spot = builder.amend_order(spot_cmd, spot_table_with_btc(), k_sign_now);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->method, net::HttpMethod::Patch);
    EXPECT_EQ(spot->body, R"({"amount":"0.5","price":"64000.1"})");
}

TEST(GateSignedRest, BatchCancelBodiesDifferPerMarket) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    auto first = limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                           QtyLots::from_raw(1));
    first.target_exchange_order_id = ExchangeOrderId::from("111").value();
    auto second = first;
    second.target_exchange_order_id = ExchangeOrderId::from("222").value();
    const std::array<OrderCmd, 2> perp_cmds{first, second};
    const auto perp = builder.cancel_batch(perp_cmds, table_with_btc(), k_sign_now);
    ASSERT_TRUE(perp.has_value());
    EXPECT_EQ(perp->target, "/api/v4/futures/usdt/batch_cancel_orders");
    EXPECT_EQ(perp->body, R"(["111","222"])");

    auto spot_cmd = limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::from_raw(640'001),
                              QtyLots::from_raw(500'000));
    spot_cmd.target_exchange_order_id = ExchangeOrderId::from("333").value();
    const std::array<OrderCmd, 1> spot_cmds{spot_cmd};
    const auto spot = builder.cancel_batch(spot_cmds, spot_table_with_btc(), k_sign_now);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(spot->target, "/api/v4/spot/cancel_batch_orders");
    EXPECT_EQ(spot->body, R"([{"currency_pair":"BTC_USDT","id":"333"}])");
}

TEST(GateSignedRest, ABatchStraddlingTwoMarketsIsRefused) {
    // One endpoint cannot carry both, and sending it anyway loses the other
    // market's cancels without saying so.
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    auto perp = limit_cmd(MarketType::Perp, Side::Buy, PriceTicks::from_raw(640'000),
                          QtyLots::from_raw(1));
    perp.target_exchange_order_id = ExchangeOrderId::from("111").value();
    auto spot = limit_cmd(MarketType::Spot, Side::Buy, PriceTicks::from_raw(640'001),
                          QtyLots::from_raw(500'000));
    spot.target_exchange_order_id = ExchangeOrderId::from("222").value();
    const std::array<OrderCmd, 2> mixed{perp, spot};
    EXPECT_FALSE(builder.cancel_batch(mixed, table_with_btc(), k_sign_now).has_value());
}

TEST(GateSignedRest, ReconciliationPullsAreScopedPerMarket) {
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};

    const auto perp_open = builder.open_orders(MarketType::Perp, k_sign_now);
    ASSERT_TRUE(perp_open.has_value());
    EXPECT_EQ(perp_open->target, "/api/v4/futures/usdt/orders?status=open&limit=1000");

    const auto spot_open = builder.open_orders(MarketType::Spot, k_sign_now);
    ASSERT_TRUE(spot_open.has_value());
    EXPECT_EQ(spot_open->target, "/api/v4/spot/open_orders?limit=100&account=spot");

    // Only the _timerange variant takes a `since`; the plain one pages by id.
    const auto fills = builder.my_trades(MarketType::Perp, UnixNanos{1'700'000'000'000'000'000ULL},
                                         k_sign_now);
    ASSERT_TRUE(fills.has_value());
    EXPECT_EQ(fills->target,
              "/api/v4/futures/usdt/my_trades_timerange?limit=1000&from=1700000000");

    const auto spot_fills = builder.my_trades(MarketType::Spot, UnixNanos{0}, k_sign_now);
    ASSERT_TRUE(spot_fills.has_value());
    EXPECT_EQ(spot_fills->target, "/api/v4/spot/my_trades?limit=1000&account=spot");

    const auto positions = builder.positions(MarketType::Perp, k_sign_now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_EQ(positions->target, "/api/v4/futures/usdt/positions?holding=true&limit=1000");

    const auto account = builder.futures_account(k_sign_now);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->target, "/api/v4/futures/usdt/accounts");

    const auto query = builder.query_order(sample_coid(), MarketType::Perp, k_sign_now);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query->target, "/api/v4/futures/usdt/orders/t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
}

TEST(GateSignedRest, OptionsAreRefusedByNameOnEveryPath) {
    // Gate lists options and the endpoints exist; nothing here maps them, and
    // the refusal has to be visible rather than a default arm's silence.
    net::Signer signer;
    load_credentials(signer);
    SignedRequestBuilder builder{signer};
    const auto table = table_with_btc();
    auto cmd = limit_cmd(MarketType::Option, Side::Buy, PriceTicks::from_raw(1),
                         QtyLots::from_raw(1));

    EXPECT_EQ(builder.create_order(cmd, table, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.cancel_order(cmd, table, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.amend_order(cmd, table, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.open_orders(MarketType::Option, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.my_trades(MarketType::Option, UnixNanos{}, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.positions(MarketType::Option, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(builder.query_order(sample_coid(), MarketType::Option, k_sign_now).error(),
              BuildError::OptionsUnsupported);
    EXPECT_EQ(to_string(BuildError::OptionsUnsupported), "gate options order entry");

    // Dated futures are a separate surface again (/api/v4/delivery), and get
    // their own name rather than being folded into the options refusal.
    auto dated = cmd;
    dated.instrument = InstrumentId::make(VenueId::Gate, MarketType::Future, SymbolId{0});
    EXPECT_EQ(builder.create_order(dated, table, k_sign_now).error(),
              BuildError::DatedFuturesUnsupported);
}

TEST(GateExecClient, EveryPullNamesTheMarketItCannotServe) {
    // not_implemented, never an empty vector: a reconciler cannot tell "flat"
    // from "this adapter has no answer" and reads the second as the first.
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Gate, .enable_perp = true}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    auto& exec_client = *adapter->execution;

    for (const auto market : {MarketType::Option, MarketType::Future}) {
        EXPECT_FALSE(exec_client.open_order_reports(market).has_value());
        EXPECT_FALSE(exec_client.fill_reports(market, UnixNanos{}).has_value());
        EXPECT_FALSE(exec_client.position_reports(market).has_value());
        EXPECT_FALSE(exec_client.query_order(sample_coid(), market).has_value());
    }
    // Spot has balances, not positions; saying "flat" would invent a concept.
    const auto spot = exec_client.position_reports(MarketType::Spot);
    ASSERT_FALSE(spot.has_value());
    EXPECT_EQ(spot.error().message.view(), exec::not_implemented("gate spot position pull")
                                               .message.view());
}

// ----------------------------------------------------- private wire ----

TEST(GateWirePrivate, FuturesPseudoStatesAreNotTerminalReasons) {
    // `_new` and `_update` appear only on the socket. Reading them as finish
    // reasons closes an order that is very much still working.
    OrderBuffer storage;
    const auto created = fixture("ws_futures_order_new.json");
    const auto opened = parse_ws_orders(created, storage, MarketType::Perp);
    ASSERT_TRUE(opened.has_value());
    ASSERT_EQ(opened->size(), 1U);
    EXPECT_EQ(opened->front().state, WireOrderState::Open);
    EXPECT_TRUE(opened->front().accepted);
    EXPECT_EQ(opened->front().contract, "BTC_USDT");
    EXPECT_EQ(opened->front().order_id, "123456789");
    EXPECT_EQ(opened->front().text, "t-01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    EXPECT_EQ(opened->front().ts_ms, 1'787'670'000'123ULL);

    const auto partial_body = fixture("ws_futures_order_partial.json");
    const auto partial = parse_ws_orders(partial_body, storage, MarketType::Perp);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->front().state, WireOrderState::Open);
    EXPECT_FALSE(partial->front().accepted);
    EXPECT_EQ(partial->front().left, "5");
}

TEST(GateWirePrivate, FuturesSizeSignBecomesTheSideAndTheMagnitudeSurvives) {
    OrderBuffer storage;
    const auto body = fixture("ws_futures_order_short.json");
    const auto orders = parse_ws_orders(body, storage, MarketType::Perp);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->size(), 1U);
    EXPECT_TRUE(orders->front().sell);
    // Keeping the sign here is how a short becomes a negative quantity three
    // layers down.
    EXPECT_EQ(orders->front().amount, "10");
    EXPECT_EQ(orders->front().left, "10");
}

TEST(GateWirePrivate, SpotEventFieldDecidesTheState) {
    OrderBuffer storage;
    struct Case {
        std::string_view file;
        WireOrderState state;
        bool accepted;
    };
    const std::array<Case, 3> cases{{
        {"ws_spot_order_put.json", WireOrderState::Open, true},
        {"ws_spot_order_finished.json", WireOrderState::Filled, false},
        {"ws_spot_order_cancelled.json", WireOrderState::Cancelled, false},
    }};
    for (const auto& one : cases) {
        const auto body = fixture(one.file);
        const auto orders = parse_ws_orders(body, storage, MarketType::Spot);
        ASSERT_TRUE(orders.has_value()) << one.file;
        ASSERT_EQ(orders->size(), 1U) << one.file;
        EXPECT_EQ(orders->front().state, one.state) << one.file;
        EXPECT_EQ(orders->front().accepted, one.accepted) << one.file;
        EXPECT_EQ(orders->front().contract, "BTC_USDT") << one.file;
    }
}

TEST(GateWirePrivate, TifThatCouldNotBeMetIsAnExpiryNotACancel) {
    OrderBuffer storage;
    const auto body = fixture("ws_futures_order_poc_rejected.json");
    const auto orders = parse_ws_orders(body, storage, MarketType::Perp);
    ASSERT_TRUE(orders.has_value());
    EXPECT_EQ(orders->front().state, WireOrderState::Expired);
}

TEST(GateWirePrivate, UserTradesCarryNoRemainingSize) {
    // The field genuinely does not exist on either API; the mapper has to get
    // leaves from the orders channel, and this test is the reason it does.
    UserTradeBuffer storage;
    const auto body = fixture("ws_futures_usertrade.json");
    const auto trades = parse_ws_user_trades(body, storage, MarketType::Perp);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ(trades->front().trade_id, "3335259");
    EXPECT_EQ(trades->front().order_id, "123456789");
    EXPECT_EQ(trades->front().qty, "5");
    EXPECT_FALSE(trades->front().sell);
    EXPECT_TRUE(trades->front().maker);
    EXPECT_EQ(trades->front().fee, "0.0032");
    EXPECT_EQ(trades->front().ts_ms, 1'787'670'001'500ULL);
}

TEST(GateWirePrivate, SpotOpenOrdersAreGroupedByPair) {
    // Reading the body as a flat order list yields nothing at all, silently.
    OrderBuffer storage;
    const auto body = fixture("rest_spot_open_orders.json");
    const auto flat = parse_rest_orders(body, storage, MarketType::Spot);
    EXPECT_FALSE(flat.has_value());

    const auto grouped = parse_spot_open_orders(body, storage);
    ASSERT_TRUE(grouped.has_value());
    ASSERT_EQ(grouped->size(), 2U);
    EXPECT_EQ((*grouped)[0].contract, "BTC_USDT");
    EXPECT_EQ((*grouped)[1].contract, "ETH_USDT");
    EXPECT_TRUE((*grouped)[1].sell);
}

TEST(GateWirePrivate, RestBodiesAreTopLevelArraysWithNoEnvelope) {
    OrderBuffer orders;
    const auto open_body = fixture("rest_futures_open_orders.json");
    const auto open = parse_rest_orders(open_body, orders, MarketType::Perp);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->size(), 2U);
    EXPECT_EQ((*open)[0].state, WireOrderState::Open);
    EXPECT_EQ((*open)[0].left, "4");
    EXPECT_TRUE((*open)[1].sell);

    const auto single_body = fixture("rest_futures_order.json");
    const auto single = parse_rest_order(single_body, MarketType::Perp);
    ASSERT_TRUE(single.has_value());
    EXPECT_EQ(single->order_id, "123456789");
    EXPECT_TRUE(single->accepted);

    PositionBuffer positions_storage;
    const auto positions_body = fixture("rest_futures_positions.json");
    const auto positions = parse_positions(positions_body, positions_storage);
    ASSERT_TRUE(positions.has_value());
    ASSERT_EQ(positions->size(), 1U);
    EXPECT_EQ(positions->front().size, "-30");

    UserTradeBuffer trades_storage;
    const auto trades_body = fixture("rest_futures_my_trades.json");
    const auto trades = parse_rest_user_trades(trades_body, trades_storage, MarketType::Perp);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 2U);
    // The REST body types create_time as FRACTIONAL seconds where the socket
    // ships integer milliseconds under a near-identical name.
    EXPECT_EQ(trades->front().ts_ms, 1'787'670'001'000ULL);
}

TEST(GateWirePrivate, TheFuturesSocketsUserIdComesFromTheAccountBody) {
    const auto body = fixture("rest_futures_account.json");
    const auto user = parse_account_user_id(body);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(*user, 10'406'147ULL);
    EXPECT_FALSE(parse_account_user_id(R"({"currency":"USDT"})").has_value());
}

// ------------------------------------------------------ private map ----

TEST(GateMapPrivate, AcceptanceBecomesAnAcceptedEvent) {
    const auto table = table_with_btc();
    OrderBuffer storage;
    const auto body = fixture("ws_futures_order_new.json");
    const auto orders = parse_ws_orders(body, storage, MarketType::Perp);
    ASSERT_TRUE(orders.has_value());

    const auto event = map_order_event(orders->front(), table, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Accepted);
    EXPECT_EQ(event->side, Side::Buy);
    EXPECT_EQ(event->qty.raw, 10);
    EXPECT_EQ(event->leaves_qty.raw, 10);
    EXPECT_EQ(event->price.raw, 640'000);
    EXPECT_EQ(event->exchange_client_order_id.view(), "01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    EXPECT_EQ(event->exchange_order_id.view(), "123456789");
    EXPECT_EQ(event->ts_event.ns, 1'787'670'000'123'000'000ULL);
}

TEST(GateMapPrivate, AFillNotificationOnTheOrderChannelEmitsNothing) {
    // The exec id and the fee live only on the user-trade channel; emitting
    // from both attributes one fill to two events.
    const auto table = table_with_btc();
    OrderBuffer storage;
    const auto partial = fixture("ws_futures_order_partial.json");
    const auto updates = parse_ws_orders(partial, storage, MarketType::Perp);
    ASSERT_TRUE(updates.has_value());
    EXPECT_FALSE(map_order_event(updates->front(), table, k_ingest).has_value());

    // A finished-and-filled order says nothing new either: the user trade that
    // took leaves to zero already carried the exec id and the fee.
    const auto finished = fixture("ws_spot_order_finished.json");
    const auto done = parse_ws_orders(finished, storage, MarketType::Spot);
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(map_order_event(done->front(), spot_table_with_btc(), k_ingest).has_value());
}

TEST(GateMapPrivate, APartialFillStatesTheLeavesTheOrderChannelSupplied) {
    const auto table = table_with_btc();
    OrderBuffer orders_storage;
    const auto order_body = fixture("ws_futures_order_partial.json");
    const auto orders = parse_ws_orders(order_body, orders_storage, MarketType::Perp);
    ASSERT_TRUE(orders.has_value());
    const auto leaves = order_leaves(orders->front(), table);
    ASSERT_TRUE(leaves.has_value());
    EXPECT_EQ(leaves->raw, 5);

    UserTradeBuffer trade_storage;
    const auto trade_body = fixture("ws_futures_usertrade.json");
    const auto trades = parse_ws_user_trades(trade_body, trade_storage, MarketType::Perp);
    ASSERT_TRUE(trades.has_value());

    const auto event =
        map_user_trade(trades->front(), table, *leaves, ExchangeClientOrderId{}, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->qty.raw, 5);
    EXPECT_EQ(event->leaves_qty.raw, 5); // partial: the order is still working
    EXPECT_EQ(event->price.raw, 640'000);
    EXPECT_EQ(event->liquidity, Liquidity::Maker);
    EXPECT_EQ(event->exec_id.view(), "3335259");
    EXPECT_EQ(event->exchange_client_order_id.view(), "01J8ZQ7X4K9M2P5R6T7V8W9XYZ");
    // Fees truncate at 1e-9 where prices refuse: a fee is neither a price nor
    // a size, and dropping the fill over its eleventh decimal would be worse.
    EXPECT_EQ(event->fee.raw, 3'200'000);
}

TEST(GateMapPrivate, AFillForAnOrderThisProcessNeverSawIsStillReported) {
    // Leaves default to zero — the fill reads as final — because losing the
    // fill entirely is the worse of the two failures, and reconciliation is
    // what settles the difference.
    const auto table = table_with_btc();
    UserTradeBuffer storage;
    const auto body = fixture("ws_futures_usertrade.json");
    const auto trades = parse_ws_user_trades(body, storage, MarketType::Perp);
    ASSERT_TRUE(trades.has_value());
    const auto event = map_user_trade(
        trades->front(), table, QtyLots::undef(), ExchangeClientOrderId{}, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->leaves_qty.raw, 0);
}

TEST(GateMapPrivate, TerminalReasonsSeparateCancelFromExpiry) {
    const auto table = table_with_btc();
    OrderBuffer storage;

    const auto cancelled_body = fixture("ws_futures_order_cancelled.json");
    const auto cancelled = parse_ws_orders(cancelled_body, storage, MarketType::Perp);
    ASSERT_TRUE(cancelled.has_value());
    const auto cancel_event = map_order_event(cancelled->front(), table, k_ingest);
    ASSERT_TRUE(cancel_event.has_value());
    EXPECT_EQ(cancel_event->type, OrderEventType::Canceled);
    EXPECT_EQ(cancel_event->leaves_qty.raw, 5);

    const auto poc_body = fixture("ws_futures_order_poc_rejected.json");
    const auto poc = parse_ws_orders(poc_body, storage, MarketType::Perp);
    ASSERT_TRUE(poc.has_value());
    const auto poc_event = map_order_event(poc->front(), table, k_ingest);
    ASSERT_TRUE(poc_event.has_value());
    // Nobody asked for the order to go away; the venue could not honour its
    // terms, and an Expired says so where a Canceled would not.
    EXPECT_EQ(poc_event->type, OrderEventType::Expired);
}

TEST(GateMapPrivate, OrdersWeDidNotPlaceStayUnattributed) {
    const auto table = table_with_btc();
    OrderBuffer storage;
    const auto body = fixture("rest_futures_open_orders.json");
    const auto orders = parse_rest_orders(body, storage, MarketType::Perp);
    ASSERT_TRUE(orders.has_value());

    const auto mine = map_open_order((*orders)[0], table);
    ASSERT_TRUE(mine.has_value());
    EXPECT_EQ(mine->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(mine->qty.raw, 10);
    EXPECT_EQ(mine->leaves.raw, 4);
    EXPECT_EQ(mine->exchange_client_order_id.view(), "01J8ZQ7X4K9M2P5R6T7V8W9XYZ");

    // `api` is Gate's own source label, not a client order id.
    const auto theirs = map_open_order((*orders)[1], table);
    ASSERT_TRUE(theirs.has_value());
    EXPECT_EQ(theirs->status, OrderStatus::Accepted);
    EXPECT_TRUE(theirs->exchange_client_order_id.empty());
    EXPECT_EQ(theirs->side, Side::Sell);
}

TEST(GateMapPrivate, PositionSignSurvivesMapping) {
    const auto table = table_with_btc();
    PositionBuffer storage;
    const auto body = fixture("rest_futures_positions.json");
    const auto positions = parse_positions(body, storage);
    ASSERT_TRUE(positions.has_value());
    const auto report = map_position(positions->front(), table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->net_qty.raw, -30);
    EXPECT_EQ(report->avg_entry.raw, 641'005);
    EXPECT_EQ(report->ts_venue.ns, 1'787'670'100'000'000'000ULL);
}

TEST(GateMapPrivate, SpotFillsMapThroughTheSameShape) {
    const auto table = spot_table_with_btc();
    UserTradeBuffer storage;
    const auto body = fixture("ws_spot_usertrade.json");
    const auto trades = parse_ws_user_trades(body, storage, MarketType::Spot);
    ASSERT_TRUE(trades.has_value());
    const auto event = map_user_trade(
        trades->front(), table, QtyLots::from_raw(200'000), ExchangeClientOrderId{}, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 300'000); // 0.3 BTC at a 0.000001 lot
    EXPECT_EQ(event->price.raw, 640'001);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->ts_event.ns, 1'787'670'011'222'000'000ULL);
}

TEST(GateMapPrivate, TheSpotFillPullReadsTheSameShapeAsTheSocket) {
    // Spot REST types create_time_ms as a STRING where the futures REST body
    // types create_time as fractional seconds; one reader has to survive both.
    const auto table = spot_table_with_btc();
    UserTradeBuffer storage;
    const auto body = fixture("rest_spot_my_trades.json");
    const auto trades = parse_rest_user_trades(body, storage, MarketType::Spot);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ(trades->front().order_id, "1036717689726");
    const auto event = map_user_trade(
        trades->front(), table, QtyLots::from_raw(200'000), ExchangeClientOrderId{}, k_ingest);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->qty.raw, 300'000);
    EXPECT_EQ(event->exec_id.view(), "7654321");
    EXPECT_EQ(event->ts_event.ns, 1'787'670'011'222'000'000ULL);
}

TEST(GateMapPrivate, SpotCatalogueTurnsPrecisionDigitsIntoSteps) {
    // Gate states spot precision as a DIGIT COUNT, not a step. Reading it as a
    // step makes every price 6 ticks wide.
    CurrencyPairBuffer storage;
    const auto body = fixture("rest_currency_pairs.json");
    const auto pairs = parse_currency_pairs(body, storage);
    ASSERT_TRUE(pairs.has_value());

    std::size_t loaded = 0;
    for (const auto& row : *pairs) {
        const auto instrument = map_currency_pair(row);
        if (row.trade_status != "tradable") {
            // buyable/sellable/untradable are one-sided or closed listings;
            // trading one places orders on the side Gate refuses.
            EXPECT_FALSE(instrument.has_value()) << row.id;
            continue;
        }
        ASSERT_TRUE(instrument.has_value()) << row.id;
        ++loaded;
        const auto& base = base_of(*instrument);
        EXPECT_EQ(base.id.market(), MarketType::Spot);
        if (row.id == "BTC_USDT") {
            EXPECT_EQ(base.tick_size.raw, 100'000'000);  // 0.1
            EXPECT_EQ(base.lot_size.raw, 1'000);         // 0.000001
        }
        if (row.id == "ALEPH_USDT") {
            EXPECT_EQ(base.lot_size.raw, k_e9); // amount_precision 0: whole units
        }
    }
    EXPECT_EQ(loaded, 3U);
}

// ------------------------------------------------------- private ws ----

TEST(GateWsPrivate, SubscribeCarriesItsOwnSignature) {
    // There is no connection-wide login for the push channels: every private
    // subscribe re-authenticates, and the `time` inside the digest must be the
    // one the frame carries.
    net::Signer signer;
    load_credentials(signer);
    Protocol protocol;
    const std::array<std::string_view, 2> payload{"10406147", k_all_symbols};
    const auto frame = protocol.private_subscribe_frame(
        orders_channel(MarketType::Perp), payload, signer, 1'541'993'715ULL);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(protocol.last_auth_message(),
              "channel=futures.orders&event=subscribe&time=1541993715");

    std::array<char, net::k_hex512_bytes> expected{};
    const auto signature =
        net::hmac_sha512_hex(k_doc_secret, protocol.last_auth_message(), expected);
    const auto text = std::string{*frame};
    EXPECT_EQ(text,
              std::string{R"({"time":1541993715,"channel":"futures.orders","event":"subscribe",)"
                          R"("payload":["10406147","!all"],"auth":{"method":"api_key","KEY":"key",)"
                          R"("SIGN":")"} +
                  std::string{signature} + R"("}})");
}

TEST(GateWsPrivate, TheFuturesPayloadLeadsWithTheUserIdAndSpotDoesNot) {
    // A futures subscribe that puts the contract where Gate expects the user
    // id is answered with an empty stream, not an error.
    net::Signer signer;
    load_credentials(signer);
    Protocol protocol;
    const std::array<std::string_view, 1> spot_payload{k_all_symbols};
    const auto spot = protocol.private_subscribe_frame(
        orders_channel(MarketType::Spot), spot_payload, signer, 1'541'993'715ULL);
    ASSERT_TRUE(spot.has_value());
    EXPECT_NE(spot->find(R"("channel":"spot.orders")"), std::string_view::npos);
    EXPECT_NE(spot->find(R"("payload":["!all"])"), std::string_view::npos);
    EXPECT_EQ(protocol.last_auth_message(), "channel=spot.orders&event=subscribe&time=1541993715");
}

TEST(GateWsPrivate, PrivateEndpointsAndChannelsExistPerMarket) {
    EXPECT_EQ(orders_channel(MarketType::Perp), "futures.orders");
    EXPECT_EQ(user_trades_channel(MarketType::Perp), "futures.usertrades");
    EXPECT_EQ(orders_channel(MarketType::Spot), "spot.orders");
    EXPECT_EQ(user_trades_channel(MarketType::Spot), "spot.usertrades");
    // Gate publishes options.orders; nothing here maps its payload, so no
    // subscription is offered rather than one that arrives unread.
    EXPECT_TRUE(orders_channel(MarketType::Option).empty());
    EXPECT_TRUE(user_trades_channel(MarketType::Option).empty());

    // Spot is a different HOST as well as a different path.
    EXPECT_EQ(private_host(false, MarketType::Perp), stream_host(false));
    EXPECT_EQ(private_path(false, MarketType::Perp), "/v4/ws/usdt");
    EXPECT_EQ(private_host(false, MarketType::Spot), "api.gateio.ws");
    EXPECT_EQ(private_path(false, MarketType::Spot), "/ws/v4/");
    EXPECT_NE(private_host(true, MarketType::Spot), private_host(false, MarketType::Spot));
}

TEST(GateWsPrivate, WithoutCredentialsNoPrivateFrameIsBuilt) {
    net::Signer signer;
    Protocol protocol;
    const std::array<std::string_view, 1> payload{k_all_symbols};
    const auto frame = protocol.private_subscribe_frame(
        orders_channel(MarketType::Spot), payload, signer, 1'541'993'715ULL);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error(), WsBuildError::NotSigned);

    const auto no_channel =
        protocol.private_subscribe_frame({}, payload, signer, 1'541'993'715ULL);
    ASSERT_FALSE(no_channel.has_value());
    EXPECT_EQ(no_channel.error(), WsBuildError::MarketUnsupported);
}

TEST(GateErrors, OrderEntryLabelsLandInTheRightBucket) {
    // A post-only that would have crossed, and a fok that could not fill, are
    // final answers about that order: retrying is how an account earns a ban.
    EXPECT_EQ(classify(k_poc_fill_immediately), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_order_poc_immediate), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_fok_not_fill), exec::ErrorClass::Terminal);
    EXPECT_EQ(classify(k_invalid_client_order_id), exec::ErrorClass::Terminal);
    // The order WAS placed and only the answer went missing; resending makes
    // a second one.
    EXPECT_EQ(classify(k_duplicate_request), exec::ErrorClass::Terminal);
    // Mid-operation on an order or a position: the same request works a
    // moment later.
    EXPECT_EQ(classify(k_order_pending), exec::ErrorClass::Retryable);
    EXPECT_EQ(classify(k_position_in_liquidation), exec::ErrorClass::Retryable);
    // Trading switched off for this key: no retry turns it back on.
    EXPECT_EQ(classify(k_trade_restricted), exec::ErrorClass::AuthFailure);
}

TEST(GateErrors, LiveRefusalBodiesClassifyAsCaptured) {
    // The body must outlive the views the parser hands back.
    const auto invalid_key_body = fixture("rest_error_invalid_key.json");
    const auto invalid_key = parse_rest_error(invalid_key_body);
    ASSERT_TRUE(invalid_key.has_value());
    EXPECT_EQ(invalid_key->label, k_invalid_key);
    EXPECT_EQ(classify(invalid_key->label), exec::ErrorClass::AuthFailure);

    // Our clock, not our credentials: halting the venue here would take a
    // healthy key offline.
    const auto expired_body = fixture("rest_error_request_expired.json");
    const auto expired = parse_rest_error(expired_body);
    ASSERT_TRUE(expired.has_value());
    EXPECT_EQ(expired->label, k_request_expired);
    EXPECT_EQ(classify(expired->label), exec::ErrorClass::Retryable);
}

// ------------------------------------------------- spot market data ----

TEST(GateSpotWire, TheSpotPrintNamesItsTakerWhereFuturesSignTheSize) {
    // Every field is spelled differently: amount for size, currency_pair for
    // the symbol, and `side` naming the TAKER outright — so there is no sign
    // to split. Read as a futures frame the same bytes have no size at all.
    auto storage = std::make_unique<TradeBuffer>();
    const auto frame = fixture("ws_spot_public_trades.json");

    EXPECT_FALSE(parse_trades(frame, *storage, MarketType::Perp).has_value());

    const auto trades = parse_trades(frame, *storage, MarketType::Spot);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U) << "spot sends one print, not a batch";
    EXPECT_EQ((*trades)[0].contract, "BTC_USDT");
    EXPECT_FALSE((*trades)[0].qty.empty());
    // create_time_ms arrives as a decimal STRING with a fraction; the
    // milliseconds are what survives.
    EXPECT_GT((*trades)[0].ts_ms, 1'000'000'000'000ULL);
    EXPECT_LT((*trades)[0].ts_ms, 100'000'000'000'000ULL);
}

TEST(GateSpotWire, TheSpotBookIsAWholeBookAndNotADelta) {
    // Levels are two-element arrays here and objects on the futures deltas.
    // Both ids are the same number because a snapshot is its own first and
    // last update — there is no chain for a consumer to join.
    auto storage = std::make_unique<LevelBuffer>();
    const auto frame = fixture("ws_spot_order_book.json");
    const auto wire = parse_book_snapshot(frame, *storage);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->contract, "BTC_USDT");
    EXPECT_EQ(wire->first_update_id, wire->last_update_id);
    ASSERT_FALSE(wire->bids.empty());
    ASSERT_FALSE(wire->asks.empty());

    const auto table = spot_table_with_btc();
    const auto book = map_book(*wire, table, UnixNanos{5});
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->instrument.market(), MarketType::Spot);
    EXPECT_GT(book->bid_count, 0U);
    EXPECT_GT(book->ask_count, 0U);
    EXPECT_LT(book->bids[0].price.raw, book->asks[0].price.raw) << "an uncrossed book";
}

TEST(GateSpotWs, TheTwoDepthChannelsTakeTheirArgumentsInOppositeOrders) {
    // Gate refuses the wrong order outright — "incorrect level provided:
    // 100ms" — so this is a startup failure rather than a quiet one, but only
    // if the frame is built per channel.
    Protocol protocol;
    const std::array<Subscription, 1> one{Subscription{.channel = "spot.order_book",
                                                       .contract = "BTC_USDT"}};
    const auto spot = protocol.subscribe_frame(one, 1);
    ASSERT_TRUE(spot.has_value());
    EXPECT_NE(spot->text.find(R"(["BTC_USDT",")" + std::string{k_depth_level} + R"(",")" +
                              std::string{k_depth_interval} + R"("])"),
              std::string_view::npos)
        << spot->text;

    const std::array<Subscription, 1> perp{Subscription{.channel = "futures.order_book_update",
                                                        .contract = "BTC_USDT"}};
    const auto futures = protocol.subscribe_frame(perp, 1);
    ASSERT_TRUE(futures.has_value());
    EXPECT_NE(futures->text.find(R"(["BTC_USDT",")" + std::string{k_depth_interval} + R"(",")" +
                                 std::string{k_depth_level} + R"("])"),
              std::string_view::npos)
        << futures->text;
}

} // namespace
} // namespace raikiri::venues::gate
