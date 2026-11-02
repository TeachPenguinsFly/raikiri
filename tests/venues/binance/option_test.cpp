#include <raikiri/domain/asset_class.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/options/chain.hpp>
#include <raikiri/options/key.hpp>
#include <raikiri/portfolio/reconcile.hpp>
#include <raikiri/venues/binance/account.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/catalogue.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/market_data.hpp>
#include <raikiri/venues/binance/orders.hpp>
#include <raikiri/venues/binance/positions.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::binance {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{RAIKIRI_BINANCE_FIXTURES} / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

FixedE9 e9(std::string_view text) {
    return *parse_decimal_e9(text);
}

constexpr std::string_view k_call = "BTC-261225-85000-C";
constexpr std::string_view k_put = "BTC-261225-85000-P";

struct Loaded {
    std::string body;
    std::size_t rows{0};
    exec::InstrumentTable table;
    std::vector<Instrument> catalogue;
};

std::unique_ptr<Loaded> load_catalogue() {
    auto out = std::make_unique<Loaded>();
    out->body = fixture("rest_option_exchange_info.json");
    static InstrumentBuffer storage;
    const auto rows = parse_option_exchange_info(out->body, storage);
    EXPECT_TRUE(rows.has_value());
    if (!rows.has_value()) {
        return out;
    }
    out->rows = rows->size();
    for (const auto& row : *rows) {
        if (auto instrument = map_instrument(row, MarketType::Option); instrument.has_value()) {
            classify(VenueId::Binance, *instrument);
            if (const auto id = out->table.add(*instrument); id.has_value()) {
                out->catalogue.push_back(*out->table.find(*id));
            }
        }
    }
    return out;
}

std::span<const WireOptionMark> marks() {
    static const auto frame = fixture("ws_option_mark.json");
    static OptionMarkBuffer storage;
    const auto data = unwrap(frame);
    EXPECT_TRUE(data.has_value());
    const auto rows = parse_option_marks(data.value_or(std::string_view{}), storage);
    EXPECT_TRUE(rows.has_value());
    return rows.value_or(std::span<const WireOptionMark>{});
}

const WireOptionMark& mark_of(std::string_view symbol) {
    for (const auto& row : marks()) {
        if (row.symbol == symbol) {
            return row;
        }
    }
    static const WireOptionMark none{};
    ADD_FAILURE() << "no mark row for " << symbol;
    return none;
}

TEST(BinanceOptionCatalogue, MapsTradingOptionsAndDropsClosedRowsAndStrikesOffTheTick) {
    const auto loaded = load_catalogue();
    // three btc, twenty doge and one closed xau
    EXPECT_EQ(loaded->rows, 24U);
    // twelve doge strikes such as 0.084 sit off the 0.01 premium tick
    ASSERT_EQ(loaded->catalogue.size(), 11U);
    EXPECT_EQ(loaded->table.find("DOGE-261009-0.084-C"), nullptr);
    EXPECT_EQ(loaded->table.find("XAU-261009-4075-C"), nullptr);

    const auto* found = loaded->table.find(k_call);
    ASSERT_NE(found, nullptr);
    const auto& call = std::get<OptionInstrument>(*found);
    EXPECT_EQ(call.put_call, PutCall::Call);
    EXPECT_EQ(call.premium, OptionUnit::Quote);
    EXPECT_EQ(call.greeks, OptionUnit::Quote);
    EXPECT_EQ(call.base.tick_size, e9("5"));
    EXPECT_EQ(call.base.lot_size, e9("0.01"));
    EXPECT_EQ(call.base.contract_value, e9("1"));
    EXPECT_EQ(call.base.underlying.view(), "BTC");
    EXPECT_EQ(call.base.asset_class, AssetClass::Crypto);
    const auto key = options::key_of(call);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(options::canonical_name(*key), "BTC-20261225-85000-C");
    EXPECT_EQ(call.expiry.ns % 86'400'000'000'000ULL, 8ULL * 3'600'000'000'000ULL);

    const auto& doge = std::get<OptionInstrument>(*loaded->table.find("DOGE-261009-0.09-P"));
    EXPECT_EQ(doge.base.contract_value, e9("1000"));
    EXPECT_EQ(options::canonical_name(*options::key_of(doge)), "DOGE-20261009-0.09-P");
}

TEST(BinanceOptionChain, PairsTheCallAndPutOfOneStrike) {
    const auto loaded = load_catalogue();
    const auto chain = options::OptionChain::build(loaded->catalogue);
    EXPECT_EQ(chain.size(), 11U);
    const auto* btc = chain.underlying("BTC");
    ASSERT_NE(btc, nullptr);
    ASSERT_EQ(btc->expiries.size(), 2U);
    const auto& december = btc->expiries[1];
    ASSERT_EQ(december.rows.size(), 1U);
    EXPECT_EQ(december.rows[0].strike, e9("85000"));
    ASSERT_TRUE(december.rows[0].call.has_value());
    ASSERT_TRUE(december.rows[0].put.has_value());
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].call), k_call);
    EXPECT_EQ(loaded->table.exchange_symbol(*december.rows[0].put), k_put);

    const auto* doge = chain.underlying("DOGE");
    ASSERT_NE(doge, nullptr);
    ASSERT_EQ(doge->expiries.size(), 1U);
    ASSERT_EQ(doge->expiries[0].rows.size(), 4U);
    EXPECT_EQ(doge->expiries[0].rows[0].strike, e9("0.08"));
    EXPECT_EQ(doge->expiries[0].rows[3].strike, e9("0.11"));
}

TEST(BinanceOptionGreeks, KeepsTheVenueConventionWhichIsOurs) {
    const auto loaded = load_catalogue();
    EXPECT_EQ(marks().size(), 20U);
    const auto greeks = map_option_greeks(mark_of("DOGE-261009-0.1-P"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(greeks.has_value());
    EXPECT_EQ(greeks->instrument, base_of(*loaded->table.find("DOGE-261009-0.1-P")).id);
    EXPECT_EQ(greeks->mark_iv, e9("0.69625616"));
    EXPECT_EQ(greeks->bid_iv, e9("0.61504364"));
    EXPECT_EQ(greeks->ask_iv, e9("0.77746868"));
    EXPECT_EQ(greeks->delta, e9("-0.78773325"));
    EXPECT_EQ(greeks->gamma, e9("39.09894638"));
    EXPECT_EQ(greeks->vega, e9("0.0000309"));
    EXPECT_EQ(greeks->theta, e9("-0.00022733"));
    EXPECT_EQ(greeks->mark_price, e9("7.2896"));
    EXPECT_EQ(greeks->index_price, e9("0.09357122"));
    EXPECT_TRUE(greeks->underlying_price.is_undef());
    EXPECT_EQ(greeks->ts_event.ns, 1'791'124'000'003'000'000ULL);

    // an ask iv of -1 says no ask
    const auto one_sided = map_option_greeks(mark_of("DOGE-261009-0.11-C"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(one_sided.has_value());
    EXPECT_TRUE(one_sided->ask_iv.is_undef());
    EXPECT_EQ(one_sided->bid_iv, e9("1.05963051"));

    EXPECT_FALSE(map_option_greeks(mark_of("DOGE-261009-0.084-P"), loaded->table, UnixNanos{1}).has_value());
}

TEST(BinanceOptionQuote, TheMarkRowQuotesUntilTheBookTickerSpeaks) {
    const auto loaded = load_catalogue();
    const auto quote = map_option_quote(mark_of("DOGE-261009-0.1-P"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->bid.raw, 705);
    EXPECT_EQ(quote->ask.raw, 755);
    EXPECT_EQ(quote->bid_qty.raw, 40'000);
    EXPECT_EQ(quote->ask_qty.raw, 3'001);

    const auto no_ask = map_option_quote(mark_of("DOGE-261009-0.11-C"), loaded->table, UnixNanos{1});
    ASSERT_TRUE(no_ask.has_value());
    EXPECT_EQ(no_ask->bid.raw, 51);
    EXPECT_TRUE(no_ask->ask.is_undef());

    // a best bid of 4.7699 is off the 0.01 tick and is dropped, never rounded
    EXPECT_FALSE(map_option_quote(mark_of("DOGE-261009-0.09-C"), loaded->table, UnixNanos{1}).has_value());
}

TEST(BinanceOptionStream, BookTickerAndTradesMapOnTheOptionSymbol) {
    const auto loaded = load_catalogue();
    const auto ticker_frame = fixture("ws_option_book_ticker.json");
    const auto ticker_data = unwrap(ticker_frame);
    ASSERT_TRUE(ticker_data.has_value());
    const auto ticker = parse_book_ticker(*ticker_data);
    ASSERT_TRUE(ticker.has_value());
    const auto bbo = map_bbo(*ticker, loaded->table, UnixNanos{1});
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->bid.raw, 1'278);
    EXPECT_EQ(bbo->ask.raw, 1'298);
    EXPECT_EQ(bbo->bid_qty.raw, 846);
    EXPECT_EQ(bbo->ts_event.ns, 1'791'123'980'077'000'000ULL);

    const auto trade_frame = fixture("ws_option_trade.json");
    const auto trade_data = unwrap(trade_frame);
    ASSERT_TRUE(trade_data.has_value());
    const auto event = parse_event_payload(*trade_data);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, EventKind::Trade);
    const auto wire = parse_trade(*trade_data);
    ASSERT_TRUE(wire.has_value());
    const auto trade = map_trade(*wire, loaded->table, UnixNanos{1});
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->instrument, base_of(*loaded->table.find("BTC-261030-86000-P")).id);
    EXPECT_EQ(trade->price.raw, 664);
    EXPECT_EQ(trade->qty.raw, 5);
    EXPECT_EQ(trade->aggressor, Side::Sell);
}

TEST(BinanceOptionStream, StreamsByUnderlyingOnTheOptionPaths) {
    std::array<char, 64> buf{};
    EXPECT_EQ(stream_name(exec::SubKind::Bbo, k_call, buf, MarketType::Option), "btc-261225-85000-c@bookTicker");
    EXPECT_EQ(stream_name(exec::SubKind::Trades, k_call, buf, MarketType::Option), "btcusdt@optionTrade");
    EXPECT_EQ(stream_name(exec::SubKind::Funding, k_call, buf, MarketType::Option), "btcusdt@optionMarkPrice");
    EXPECT_TRUE(stream_name(exec::SubKind::Depth, k_call, buf, MarketType::Option).empty());
    EXPECT_EQ(stream_name(exec::SubKind::Trades, "BTCUSDT", buf), "btcusdt@trade");
    EXPECT_EQ(stream_path(exec::SubKind::Bbo, MarketType::Option), "/public/stream");
    EXPECT_EQ(stream_path(exec::SubKind::Funding, MarketType::Option), "/market/stream");
    EXPECT_EQ(stream_path(exec::SubKind::Bbo), "/stream");
    EXPECT_EQ(ep::rest_host(false, MarketType::Option), "eapi.binance.com");

    net::Signer signer;
    RequestBuilder builder{signer, MarketType::Option};
    EXPECT_EQ(builder.instruments().target, "/eapi/v1/exchangeInfo");
    EXPECT_EQ(builder.server_time().target, "/eapi/v1/time");
}

TEST(BinanceOptionAccount, TheEapiAccountIsAnOptionsWalletOfItsOwn) {
    net::Signer signer;
    signer.set_api_key("TESTKEY");
    ASSERT_TRUE(signer.secret().assign("testsecret").has_value());
    RequestBuilder builder{signer, MarketType::Option};
    const auto request = builder.option_account(UnixNanos{1'592'449'455'000'000'000ULL});
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->target.starts_with("/eapi/v1/account?recvWindow="));
    EXPECT_NE(request->target.find("&signature="), std::string_view::npos);
    EXPECT_EQ(rest_host(false, MarketType::Option), "eapi.binance.com");

    const auto body = fixture("rest_option_account.json");
    OptionAccountBuffer storage;
    const auto rows = parse_option_account(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    const auto usdt = map_option_balance((*rows)[0], UnixNanos{9});
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->asset.view(), "USDT");
    EXPECT_EQ(usdt->wallet, WalletKind::Options);
    EXPECT_EQ(usdt->total, e9("617.77711415"));
    EXPECT_EQ(usdt->available, e9("0"));
    EXPECT_EQ(usdt->unrealized, e9("222.23697"));
    EXPECT_FALSE(map_option_balance((*rows)[1], UnixNanos{9}).has_value()) << "nothing held";
    EXPECT_TRUE(exec::caps_of(VenueId::Binance).balance_pull.has(MarketType::Option));
}

constexpr std::string_view k_coid = "01JBZ3NDEKTSV4RRFFQ69G5OPT";

OrderCmd option_order(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(k_call)).id;
    cmd.side = Side::Sell;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    cmd.price = PriceTicks::from_raw(301); // 1505 at a 5 tick
    cmd.qty = QtyLots::from_raw(5);        // 0.05 contracts at a 0.01 lot
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from(k_coid);
    return cmd;
}

struct Signed {
    net::Signer signer;
    RequestBuilder builder;

    Signed()
        : builder{(signer.set_api_key("TESTKEY"), (void)signer.secret().assign("testsecret"), signer),
                  MarketType::Option} {}
};

constexpr UnixNanos k_now{1'792'465'880'000'000'000ULL};

TEST(BinanceOptionOrders, CreateSendsContractsLimitOnlyAndGtxOnEapi) {
    const auto loaded = load_catalogue();
    const auto signed_up = std::make_unique<Signed>();
    auto& builder = signed_up->builder;
    const auto& base = base_of(*loaded->table.find(k_call));

    const auto create = builder.create_order(option_order(loaded->table), base, k_now);
    ASSERT_TRUE(create.has_value());
    EXPECT_EQ(create->method, net::HttpMethod::Post);
    EXPECT_TRUE(create->target.starts_with("/eapi/v1/order?symbol=BTC-261225-85000-C&side=SELL&type=LIMIT"
                                           "&timeInForce=GTX&quantity=0.05&price=1505"
                                           "&clientOrderId=01JBZ3NDEKTSV4RRFFQ69G5OPT&recvWindow=5000"
                                           "&timestamp=1792465880000&signature="))
        << create->target;

    auto reduce = option_order(loaded->table);
    reduce.flags = OrderFlags::ReduceOnly;
    reduce.tif = TimeInForce::Ioc;
    const auto closing = builder.create_order(reduce, base, k_now);
    ASSERT_TRUE(closing.has_value());
    EXPECT_NE(closing->target.find("&timeInForce=IOC"), std::string_view::npos);
    EXPECT_NE(closing->target.find("&reduceOnly=true"), std::string_view::npos);

    auto market = option_order(loaded->table);
    market.flags = OrderFlags::None;
    market.order_type = OrderType::Market;
    EXPECT_EQ(builder.create_order(market, base, k_now).error(), BuildError::UnsupportedOrderType);
}

TEST(BinanceOptionOrders, CancelNamesTheOrderByTheOptionParametersAndAmendIsRefused) {
    const auto loaded = load_catalogue();
    const auto signed_up = std::make_unique<Signed>();
    auto& builder = signed_up->builder;
    const auto& base = base_of(*loaded->table.find(k_call));

    auto cancel = option_order(loaded->table);
    cancel.kind = CmdKind::Cancel;
    const auto by_client = builder.cancel_order(cancel, base, k_now);
    ASSERT_TRUE(by_client.has_value());
    EXPECT_EQ(by_client->method, net::HttpMethod::Delete);
    EXPECT_TRUE(by_client->target.starts_with("/eapi/v1/order?symbol=BTC-261225-85000-C"
                                              "&clientOrderId=01JBZ3NDEKTSV4RRFFQ69G5OPT&recvWindow="))
        << by_client->target;

    cancel.target_exchange_order_id = *ExchangeOrderId::from("4611875134427365000");
    const auto by_venue = builder.cancel_order(cancel, base, k_now);
    ASSERT_TRUE(by_venue.has_value());
    EXPECT_NE(by_venue->target.find("&orderId=4611875134427365000&"), std::string_view::npos);

    EXPECT_EQ(builder.amend_order(cancel, base, k_now).error(), BuildError::UnsupportedOrderType);
}

TEST(BinanceOptionOrders, PullsAndTheListenKeyGoToEapiAndAGetCarriesNoContentType) {
    const auto signed_up = std::make_unique<Signed>();
    auto& builder = signed_up->builder;

    const auto open = builder.open_orders(k_now);
    ASSERT_TRUE(open.has_value());
    EXPECT_TRUE(open->target.starts_with("/eapi/v1/openOrders?"));
    EXPECT_TRUE(open->content_type.empty());
    EXPECT_TRUE(builder.position_risk(k_now)->target.starts_with("/eapi/v1/position?"));

    // the endpoint takes one symbol, so the whole-account form of the perp path is refused
    EXPECT_EQ(builder.user_trades({}, UnixNanos{}, k_now).error(), BuildError::BadValue);
    const auto trades = builder.user_trades(k_call, UnixNanos{1'792'000'000'000'000'000ULL}, k_now);
    ASSERT_TRUE(trades.has_value());
    EXPECT_TRUE(trades->target.starts_with("/eapi/v1/userTrades?symbol=BTC-261225-85000-C"
                                           "&startTime=1792000000000&limit=1000&recvWindow="))
        << trades->target;

    const auto bill = builder.option_bill(UnixNanos{1'792'000'000'000'000'000ULL}, k_now);
    ASSERT_TRUE(bill.has_value());
    EXPECT_TRUE(bill->target.starts_with("/eapi/v1/bill?currency=USDT&limit=1000&startTime=1792000000000&"))
        << bill->target;

    EXPECT_EQ(builder.listen_key_create()->target, "/eapi/v1/listenKey");
    EXPECT_EQ(builder.listen_key_keepalive()->method, net::HttpMethod::Put);
    EXPECT_EQ(builder.listen_key_keepalive()->target, "/eapi/v1/listenKey");
    EXPECT_EQ(builder.listen_key_close()->target, "/eapi/v1/listenKey");

    net::Signer perp_signer;
    perp_signer.set_api_key("TESTKEY");
    ASSERT_TRUE(perp_signer.secret().assign("testsecret").has_value());
    RequestBuilder perp{perp_signer};
    EXPECT_EQ(perp.listen_key_create()->target, "/fapi/v1/listenKey");
    EXPECT_EQ(perp.open_orders(k_now)->content_type, "application/x-www-form-urlencoded");
}

TEST(BinanceOptionStream, OrderFramesMapToTheOptionWithAPositiveFeeInItsCurrency) {
    const auto loaded = load_catalogue();
    const auto id = base_of(*loaded->table.find(k_call)).id;

    const auto accepted_body = fixture("ws_option_order_new.json");
    const auto kind = parse_event_payload(accepted_body);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(kind->kind, EventKind::OrderUpdate);
    const auto accepted_wire = parse_order_update(accepted_body);
    ASSERT_TRUE(accepted_wire.has_value());
    const auto accepted = map_order_event(*accepted_wire, loaded->table, k_now);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->instrument, id);
    EXPECT_EQ(accepted->side, Side::Sell);
    EXPECT_TRUE(has_flag(accepted->flags, OrderFlags::PostOnly));
    EXPECT_EQ(accepted->price, PriceTicks::from_raw(301));
    EXPECT_EQ(accepted->qty, QtyLots::from_raw(5));
    EXPECT_EQ(accepted->leaves_qty, QtyLots::from_raw(5));
    EXPECT_EQ(accepted->exchange_client_order_id.view(), k_coid);
    EXPECT_EQ(accepted->exchange_order_id.view(), "4611875134427365000");

    const auto fill_body = fixture("ws_option_order_fill.json");
    const auto fill_wire = parse_order_update(fill_body);
    ASSERT_TRUE(fill_wire.has_value());
    const auto fill = map_order_event(*fill_wire, loaded->table, k_now);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->qty, QtyLots::from_raw(1));
    EXPECT_EQ(fill->leaves_qty, QtyLots::from_raw(4));
    EXPECT_EQ(fill->price, PriceTicks::from_raw(301));
    EXPECT_EQ(fill->liquidity, Liquidity::Maker);
    // eapi writes a charge negative; the book counts a charge as positive
    EXPECT_EQ(fill->fee, e9("0.3"));
    EXPECT_EQ(fill->fee_asset.view(), "USDT");
    EXPECT_EQ(fill->exec_id.view(), "239-4611875134427365000");
}

TEST(BinanceOptionStream, TheWrappedFormOfAFrameUnwrapsToTheSameEvent) {
    const std::string wrapped = R"({"stream":"listenkey","data":)" + fixture("ws_option_order_fill.json") + "}";
    const auto payload = unwrap(wrapped);
    ASSERT_TRUE(payload.has_value());
    const auto kind = parse_event_payload(*payload);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(kind->kind, EventKind::OrderUpdate);
    EXPECT_TRUE(parse_order_update(*payload).has_value());
}

TEST(BinanceOptionRecon, OpenOrdersReadTheQuantityFieldAndItsFills) {
    const auto loaded = load_catalogue();
    const auto body = fixture("rest_option_open_orders.json");
    OrderBuffer storage;
    const auto rows = parse_option_orders(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto report = map_open_order((*rows)[0], loaded->table);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->qty, QtyLots::from_raw(5));
    EXPECT_EQ(report->leaves, QtyLots::from_raw(2));
    EXPECT_EQ(report->exchange_client_order_id.view(), k_coid);

    const auto placed = fixture("rest_option_order.json");
    EXPECT_TRUE(parse_rest_error(placed).has_value());
    EXPECT_EQ(parse_rest_error(placed)->code, k_ok);
}

TEST(BinanceOptionRecon, TheBillAndThePositionsSayWhichSymbolsToAskOnce) {
    const auto bill_body = fixture("rest_option_bill.json");
    OptionBillBuffer bill_storage;
    const auto bill = parse_option_bill(bill_body, bill_storage);
    ASSERT_TRUE(bill.has_value());
    ASSERT_EQ(bill->size(), 4U);
    EXPECT_TRUE((*bill)[2].symbol.empty());

    const auto positions_body = fixture("rest_option_positions.json");
    std::array<WirePositionRisk, 8> position_storage{};
    const auto positions = parse_option_positions(positions_body, position_storage);
    ASSERT_TRUE(positions.has_value());

    const auto symbols = option_traded_symbols(*bill, *positions);
    ASSERT_EQ(symbols.size(), 2U);
    EXPECT_EQ(symbols[0], k_call);
    EXPECT_EQ(symbols[1], k_put);
}

TEST(BinanceOptionRecon, ThePulledHistoryFindsTheFillTheSocketMissedAndThePositionThenAgrees) {
    const auto loaded = load_catalogue();
    const auto id = base_of(*loaded->table.find(k_call)).id;
    oms::SeenFills seen;
    QtyLots local = QtyLots::from_raw(0);

    const auto frame = fixture("ws_option_order_fill.json");
    const auto pushed = map_order_event(*parse_order_update(frame), loaded->table, k_now);
    ASSERT_TRUE(pushed.has_value());
    ASSERT_TRUE(seen.admit(pushed->exec_id.view(), k_now));
    local = QtyLots::from_raw(local.raw - pushed->qty.raw);

    const auto positions_body = fixture("rest_option_positions.json");
    std::array<WirePositionRisk, 8> position_storage{};
    const auto rows = parse_option_positions(positions_body, position_storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto venue = map_position((*rows)[0], loaded->table);
    ASSERT_TRUE(venue.has_value());
    EXPECT_EQ(venue->net_qty, QtyLots::from_raw(-3));
    EXPECT_EQ(venue->avg_entry, PriceTicks::from_raw(301));
    EXPECT_EQ(portfolio::reconcile_position(true, venue, local).verdict, portfolio::ReconVerdict::Break);

    const auto history_body = fixture("rest_option_user_trades.json");
    TradeBuffer history_storage;
    const auto history = parse_option_user_trades(history_body, history_storage);
    ASSERT_TRUE(history.has_value());
    ASSERT_EQ(history->size(), 2U);
    std::vector<exec::FillReport> missed;
    for (const auto& wire : *history) {
        if (const auto report = map_user_trade(wire, loaded->table);
            report.has_value() && seen.admit(report->exec_id.view(), k_now)) {
            missed.push_back(*report);
        }
    }
    // the pull's id for the pushed fill matches the push, so only the missed one is new
    ASSERT_EQ(missed.size(), 1U);
    EXPECT_EQ(missed[0].exec_id.view(), "240-4611875134427365000");
    EXPECT_EQ(missed[0].instrument, id);
    EXPECT_EQ(missed[0].side, Side::Sell);
    EXPECT_EQ(missed[0].fee, e9("0.6"));
    EXPECT_EQ(missed[0].fee_asset.view(), "USDT");
    EXPECT_EQ(missed[0].liquidity, Liquidity::Taker);
    local = QtyLots::from_raw(local.raw - missed[0].qty.raw);
    EXPECT_EQ(portfolio::reconcile_position(true, venue, local).verdict, portfolio::ReconVerdict::Agree);
}

TEST(BinanceOptionRecon, TheSameTradeNumberOnAnotherOrderIsAnotherExecution) {
    const auto loaded = load_catalogue();
    WireTrade first{};
    first.symbol = k_call;
    first.price = "1505";
    first.qty = "0.01";
    first.side = "BUY";
    first.trade_id = 239;
    first.order_id = 1;
    auto second = first;
    second.symbol = k_put;
    second.order_id = 2;
    const auto one = map_user_trade(first, loaded->table);
    const auto two = map_user_trade(second, loaded->table);
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    EXPECT_NE(one->exec_id, two->exec_id);
}

TEST(BinanceOptionAccount, TheMarginAccountIsTheConfiguredAlternative) {
    const auto signed_up = std::make_unique<Signed>();
    const auto request = signed_up->builder.option_account(k_now, true);
    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->target.starts_with("/eapi/v1/marginAccount?recvWindow=")) << request->target;

    const auto body = fixture("rest_option_margin_account.json");
    OptionAccountBuffer storage;
    const auto rows = parse_option_account(body, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U);
    const auto usdt = map_option_balance((*rows)[0], UnixNanos{9});
    ASSERT_TRUE(usdt.has_value());
    EXPECT_EQ(usdt->asset.view(), "USDT");
    EXPECT_EQ(usdt->wallet, WalletKind::Options);
    EXPECT_EQ(usdt->total, e9("10094.44662"));
    EXPECT_EQ(usdt->available, e9("8725.92524"));
    EXPECT_EQ(usdt->unrealized, e9("-5.00138"));
}

TEST(BinanceOptionCaps, PullsAreDeclaredForOptions) {
    const auto caps = exec::caps_of(VenueId::Binance);
    EXPECT_TRUE(caps.position_pull.has(MarketType::Option));
    EXPECT_TRUE(caps.order_pull.has(MarketType::Option));
    EXPECT_TRUE(caps.fill_pull.has(MarketType::Option));
    EXPECT_TRUE(caps.balance_pull.has(MarketType::Option));
    EXPECT_FALSE(caps.funding_charge.has(MarketType::Option));
}

} // namespace
} // namespace raikiri::venues::binance
