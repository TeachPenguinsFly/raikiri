#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/portfolio/reconcile.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/catalogue.hpp>
#include <raikiri/venues/bybit/envelope.hpp>
#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/market_data.hpp>
#include <raikiri/venues/bybit/orders.hpp>
#include <raikiri/venues/bybit/positions.hpp>
#include <raikiri/venues/bybit/rest.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::bybit {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

FixedE9 e9(std::string_view text) {
    return *parse_decimal_e9(text);
}

constexpr std::string_view k_call = "BTC-26MAR27-90000-C-USDT";
constexpr std::string_view k_put = "BTC-26MAR27-90000-P-USDT";

struct Loaded {
    std::string body;
    std::size_t rows{0};
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_instruments_option.json");
    const auto envelope = parse_envelope(out->body);
    EXPECT_TRUE(envelope.has_value());
    if (!envelope.has_value()) {
        return out;
    }
    static InstrumentBuffer storage;
    const auto rows = parse_instruments(envelope->result, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    out->rows = rows->size();
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Option); instrument.has_value()) {
            classify(VenueId::Bybit, *instrument);
            if (const auto id = out->table.add(*instrument); id.has_value()) {
                out->catalogue.push_back(*out->table.find(*id));
            }
        }
    }
    return out;
}

TEST(BybitOptionCatalogue, MapsUsdtOptionsAndDropsNamesPastTheSymbolWidth) {
    const auto loaded = load_catalogue();
    EXPECT_EQ(loaded->rows, 4U);
    // BTC-25JUN27-106000-P-USDT is 25 bytes, one past SymbolText
    ASSERT_EQ(loaded->catalogue.size(), 2U);

    const auto* found = loaded->table.find(k_call);
    ASSERT_NE(found, nullptr);
    const auto& call = std::get<OptionInstrument>(*found);
    EXPECT_EQ(call.put_call, PutCall::Call);
    EXPECT_EQ(call.premium, OptionUnit::Quote);
    EXPECT_EQ(call.greeks, OptionUnit::Quote);
    EXPECT_EQ(call.base.tick_size, e9("5"));
    EXPECT_EQ(call.base.lot_size, e9("0.01"));
    EXPECT_EQ(call.base.underlying.view(), "BTC");
    EXPECT_EQ(call.base.id.market(), MarketType::Option);
    const auto key = options::key_of(call);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20270326-90000-C");
    EXPECT_EQ(call.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);
}

TEST(BybitOptionCatalogue, ReadsAFractionalStrikeFromTheSymbol) {
    WireInstrument row{};
    row.symbol = "XRP-4OCT26-1.1-C-USDT";
    row.base_coin = "XRP";
    row.quote_coin = "USDT";
    row.status = "Trading";
    row.tick_size = "0.0001";
    row.qty_step = "10";
    row.options_type = "Call";
    row.delivery_ms = 1'791'100'800'000ULL;
    const auto mapped = map_instrument(row, MarketType::Option);
    ASSERT_TRUE(mapped.has_value());
    auto instrument = *mapped;
    classify(VenueId::Bybit, instrument);
    const auto key = options::key_of(std::get<OptionInstrument>(instrument));
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "XRP-20261004-1.1-C");

    row.options_type = "";
    EXPECT_FALSE(map_instrument(row, MarketType::Option).has_value());
    row.options_type = "Put";
    row.symbol = "XRP-4OCT26-1.10005-P-USDT";
    EXPECT_FALSE(map_instrument(row, MarketType::Option).has_value());
}

TEST(BybitOptionChain, PairsTheCallAndPutOfOneStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 2U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 1U);
    ASSERT_EQ(btc->expiries[0].rows.size(), 1U);
    const auto& row = btc->expiries[0].rows[0];
    EXPECT_EQ(row.strike, e9("90000"));
    ASSERT_TRUE(row.call.has_value());
    ASSERT_TRUE(row.put.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*row.call), k_call);
    EXPECT_EQ(loaded->table.exchange_symbol(*row.put), k_put);
}

TEST(BybitOptionGreeks, KeepsTheVenueConventionWhichIsOurs) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("option_tickers.json");
    const auto topic = parse_topic(frame);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(topic->kind, TopicKind::Tickers);

    const auto wire = parse_option_ticker(frame);
    ASSERT_TRUE(wire.has_value());
    EXPECT_EQ(wire->symbol, k_call);
    const auto greeks = map_option_greeks(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find(k_call)).id);
    EXPECT_EQ(greeks->delta, e9("0.49801447"));
    EXPECT_EQ(greeks->gamma, e9("0.00001782"));
    EXPECT_EQ(greeks->vega, e9("239.22969112"));
    EXPECT_EQ(greeks->theta, e9("-25.72991624"));
    EXPECT_EQ(greeks->mark_iv, e9("0.3735"));
    EXPECT_EQ(greeks->bid_iv, e9("0.372"));
    EXPECT_EQ(greeks->ask_iv, e9("0.3787"));
    EXPECT_EQ(greeks->underlying_price, e9("86953"));
    EXPECT_EQ(greeks->index_price, e9("84899.07664735"));
    EXPECT_EQ(greeks->mark_price, e9("7621.88934212"));
    EXPECT_EQ(greeks->ts_event.ns, 1'791'049'140'169'000'000ULL);

    auto no_bid = *wire;
    no_bid.bid_iv = "0";
    no_bid.delta = "";
    const auto empty_side = map_option_greeks(no_bid, loaded->table, UnixNanos{1});
    ASSERT_TRUE(empty_side.has_value());
    EXPECT_TRUE(empty_side->bid_iv.is_undef());
    EXPECT_TRUE(empty_side->delta.is_undef());

    no_bid.symbol = "BTC-26MAR27-1-C-USDT";
    EXPECT_FALSE(map_option_greeks(no_bid, loaded->table, UnixNanos{1}).has_value());
}

TEST(BybitOptionStream, QuotesFromTheTwentyFiveLevelBook) {
    const auto loaded = load_catalogue();
    const auto frame = fixture("option_orderbook_snapshot.json");
    static OrderbookBuffer storage;
    const auto book = parse_orderbook(frame, storage);
    ASSERT_TRUE(book.has_value());
    EXPECT_EQ(book->symbol, k_call);
    const auto& base = base_of(*loaded->table.find(k_call));
    BookSync sync{base.id, base.tick_size, base.lot_size};
    EXPECT_EQ(sync.apply(*book), SyncResult::Resynced);
    const auto bbo = sync.bbo(UnixNanos{2}, UnixNanos{3});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 1518);
    EXPECT_EQ(bbo->ask.raw, 1550);
    EXPECT_EQ(bbo->bid_qty.raw, 174);
}

TEST(BybitOptionStream, TradesArriveOnTheBaseCoinTopic) {
    const auto frame = fixture("option_public_trade.json");
    static TradeBuffer storage;
    const auto trades = parse_trades(frame, storage);
    ASSERT_TRUE(trades.has_value());
    ASSERT_EQ(trades->size(), 1U);
    EXPECT_EQ((*trades)[0].symbol, "BTC-4OCT26-85750-C-USDT");
    EXPECT_EQ((*trades)[0].price, "20");
    EXPECT_TRUE((*trades)[0].buyer_aggressor);

    std::array<char, 64> buf{};
    EXPECT_EQ(topic_name(exec::SubKind::Trades, "BTC", buf), "publicTrade.BTC");
    EXPECT_EQ(topic_name(exec::SubKind::Bbo, k_call, buf, k_option_book_depth),
              "orderbook.25.BTC-26MAR27-90000-C-USDT");
    EXPECT_EQ(public_stream_path(MarketType::Option), "/v5/public/option");
}

OrderCmd option_order(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(k_call)).id;
    cmd.side = Side::Sell;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.price = PriceTicks::from_raw(301); // 1505 at a 5 tick
    cmd.qty = QtyLots::from_raw(5);        // 0.05 btc at a 0.01 lot
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01JBZ3NDEKTSV4RRFFQ69G5OPT");
    return cmd;
}

TEST(BybitOptionOrders, CreateSendsTheOptionCategoryContractsAndPostOnly) {
    const auto loaded = load_catalogue();
    net::Signer signer;
    signer.set_api_key("TESTKEY123");
    ASSERT_TRUE(signer.secret().assign("testsecret").has_value());
    RequestBuilder builder{signer};
    const UnixNanos now{1'774'500'000'000'000'000ULL};

    const auto create = builder.create_order(option_order(loaded->table), loaded->table, now);
    ASSERT_TRUE(create.has_value()) << to_string(create.error());
    EXPECT_EQ(create->body,
              R"({"category":"option","symbol":"BTC-26MAR27-90000-C-USDT","side":"Sell","orderType":"Limit",)"
              R"("qty":"0.05","price":"1505","timeInForce":"PostOnly",)"
              R"("orderLinkId":"01JBZ3NDEKTSV4RRFFQ69G5OPT"})");

    auto amend = option_order(loaded->table);
    amend.kind = CmdKind::Amend;
    amend.target_exchange_order_id = *ExchangeOrderId::from("8b1c3a2e-5f4d-4e3b-9c2a-1d0e9f8a7b6c");
    amend.price = PriceTicks::from_raw(302);
    const auto amended = builder.amend_order(amend, loaded->table, now);
    ASSERT_TRUE(amended.has_value());
    EXPECT_EQ(amended->body,
              R"({"category":"option","symbol":"BTC-26MAR27-90000-C-USDT",)"
              R"("orderId":"8b1c3a2e-5f4d-4e3b-9c2a-1d0e9f8a7b6c","qty":"0.05","price":"1510"})");

    // settleCoin would hide the usdc options from every pull
    EXPECT_EQ(builder.open_orders(MarketType::Option, now)->target, "/v5/order/realtime?category=option");
    EXPECT_EQ(builder.positions(MarketType::Option, now)->target, "/v5/position/list?category=option");
    EXPECT_EQ(builder.cancel_all(MarketType::Option, now)->body, R"({"category":"option"})");
    EXPECT_NE(builder.open_orders(MarketType::Perp, now)->target.find("settleCoin=USDT"), std::string_view::npos);
}

TEST(BybitOptionStream, OrderAndExecutionFramesMapToTheOptionInContracts) {
    const auto loaded = load_catalogue();
    const auto id = base_of(*loaded->table.find(k_call)).id;
    const UnixNanos received{1'774'500'000'200'000'000ULL};

    const auto orders = fixture("option_order_update.json");
    OrderUpdateBuffer order_storage;
    const auto updates = parse_order_updates(orders, order_storage);
    ASSERT_TRUE(updates.has_value());
    ASSERT_EQ(updates->size(), 2U);
    // a partial fill is left to the execution row
    EXPECT_FALSE(map_order_event((*updates)[0], loaded->table, received).has_value());
    const auto accepted = map_order_event((*updates)[1], loaded->table, received);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->instrument, id);
    EXPECT_EQ(accepted->price, PriceTicks::from_raw(240));
    EXPECT_EQ(accepted->qty, QtyLots::from_raw(10));
    EXPECT_EQ(accepted->leaves_qty, QtyLots::from_raw(10));

    const auto frame = fixture("option_execution.json");
    ExecutionBuffer exec_storage;
    const auto execs = parse_executions(frame, exec_storage);
    ASSERT_TRUE(execs.has_value());
    ASSERT_EQ(execs->size(), 1U);
    const auto fill = map_execution((*execs)[0], loaded->table, received);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->instrument, id);
    EXPECT_EQ(fill->side, Side::Sell);
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    EXPECT_EQ(fill->price, PriceTicks::from_raw(301));
    EXPECT_EQ(fill->qty, QtyLots::from_raw(1));
    EXPECT_EQ(fill->leaves_qty, QtyLots::from_raw(4));
    EXPECT_EQ(fill->fee, e9("0.3"));
    EXPECT_EQ(fill->exchange_client_order_id.view(), "01JBZ3NDEKTSV4RRFFQ69G5OPT");
}

TEST(BybitOptionRecon, ThePulledHistoryFindsTheFillTheSocketMissedAndThePositionThenAgrees) {
    const auto loaded = load_catalogue();
    const auto id = base_of(*loaded->table.find(k_call)).id;
    const UnixNanos now{1'774'500'000'300'000'000ULL};
    oms::SeenFills seen;
    QtyLots local = QtyLots::from_raw(0);

    const auto frame = fixture("option_execution.json");
    ExecutionBuffer stream_storage;
    const auto pushed = map_execution((*parse_executions(frame, stream_storage))[0], loaded->table, now);
    ASSERT_TRUE(pushed.has_value());
    ASSERT_TRUE(seen.admit(pushed->exec_id.view(), now));
    local = QtyLots::from_raw(local.raw - pushed->qty.raw);

    const auto positions_body = fixture("rest_positions_option.json");
    const auto positions_envelope = parse_envelope(positions_body);
    ASSERT_TRUE(positions_envelope.has_value());
    PositionBuffer position_storage;
    const auto rows = parse_position_list(positions_envelope->result, position_storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto venue = map_position((*rows)[0], loaded->table);
    ASSERT_TRUE(venue.has_value());
    EXPECT_EQ(venue->net_qty, QtyLots::from_raw(-2));
    EXPECT_EQ(venue->avg_entry, PriceTicks::from_raw(301));
    EXPECT_EQ(portfolio::reconcile_position(true, venue, local).verdict, portfolio::ReconVerdict::Break);

    const auto history_body = fixture("rest_executions_option.json");
    const auto history_envelope = parse_envelope(history_body);
    ASSERT_TRUE(history_envelope.has_value());
    ExecutionBuffer history_storage;
    const auto history = parse_execution_list(history_envelope->result, history_storage);
    ASSERT_TRUE(history.has_value());
    ASSERT_EQ(history->size(), 3U);
    std::vector<OrderEvent> missed;
    for (const auto& wire : *history) {
        // the delivery row is not a fill
        if (const auto event = map_execution(wire, loaded->table, now);
            event.has_value() && seen.admit(event->exec_id.view(), now)) {
            missed.push_back(*event);
        }
    }
    ASSERT_EQ(missed.size(), 1U);
    EXPECT_EQ(missed[0].exec_id.view(), "a1b2c3d4-0002-5800-a352-893d52b446aa");
    EXPECT_EQ(missed[0].instrument, id);
    local = QtyLots::from_raw(local.raw - missed[0].qty.raw);
    EXPECT_EQ(portfolio::reconcile_position(true, venue, local).verdict, portfolio::ReconVerdict::Agree);
}

} // namespace
} // namespace raikiri::venues::bybit
