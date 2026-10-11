#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/venues/lighter/account.hpp>
#include <raikiri/venues/lighter/catalogue.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/venues/lighter/lifecycle.hpp>
#include <raikiri/venues/lighter/map.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace raikiri::venues::lighter {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr UnixNanos k_now{1'791'726'900'000'000'000ULL};
constexpr std::int64_t k_account = 27927;

class CaptureSink final : public exec::GatewaySink {
public:
    void on_bbo(const Bbo&) noexcept override {}

    void on_trade(const TradeTick&) noexcept override {}

    void on_funding(const FundingUpdate&) noexcept override {}

    void on_order_event(const OrderEvent& event) noexcept override { events.push_back(event); }

    void on_order_error(ClientOrderId, const exec::VenueError& error) noexcept override { errors.push_back(error); }

    void on_connectivity(const exec::ConnEvent&) noexcept override {}

    std::vector<OrderEvent> events;
    std::vector<exec::VenueError> errors;
};

// btc, market 1: ticks of 0.1, lots of 0.00001
InstrumentId load_btc(exec::InstrumentTable& table) {
    static const auto body = fixture("rest_order_book_details.json");
    static MarketBuffer storage;
    const auto rows = parse_order_book_details(body, storage);
    EXPECT_TRUE(rows.has_value());
    for (const auto& row : *rows) {
        if (row.market_id == 1) {
            const auto instrument = map_instrument(row, MarketType::Perp);
            EXPECT_TRUE(instrument.has_value());
            const auto id = table.add(*instrument);
            EXPECT_TRUE(id.has_value());
            return *id;
        }
    }
    ADD_FAILURE() << "no btc row";
    return {};
}

ExchangeClientOrderId ulid(std::string_view text) {
    const auto id = ExchangeClientOrderId::from(text);
    EXPECT_TRUE(id.has_value());
    return id.value_or(ExchangeClientOrderId{});
}

OrderCmd ioc_buy(InstrumentId instrument, std::int64_t lots) {
    OrderCmd cmd{};
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Ioc;
    cmd.instrument = instrument;
    cmd.client_order_id = ClientOrderId::make(7, 1);
    cmd.exchange_client_order_id = ulid("01K7AB3C4D5E6F7G8H9J0KMNPQ");
    cmd.price = PriceTicks::from_raw(830300);
    cmd.qty = QtyLots::from_raw(lots);
    return cmd;
}

// the row the venue's own sdk models, at the fields the gateway reads
std::string orders_frame(std::int64_t client_index, std::string_view status, std::string_view filled) {
    return R"({"channel":"account_all_orders:27927","orders":{"1":[{"order_index":844421320500417,)"
           R"("client_order_index":)" +
           std::to_string(client_index) +
           R"(,"order_id":"844421320500417","market_index":1,"owner_account_index":27927,)"
           R"("initial_base_amount":"0.00300","price":"83030.0","nonce":1791726881000,"remaining_base_amount":"0.00000",)"
           R"("is_ask":false,"filled_base_amount":")" +
           std::string{filled} +
           R"(","filled_quote_amount":"147.779516","type":"limit",)"
           R"("time_in_force":"immediate-or-cancel","reduce_only":false,"status":")" +
           std::string{status} + R"(","updated_at":1791726881383}]},"type":"update/account_all_orders"})";
}

// the recorded print with our account on the bid and our client index on it
std::string trades_frame(std::int64_t client_index) {
    auto body = fixture("rest_trades.json");
    const auto replace = [&](std::string_view from, std::string_view to) {
        const auto at = body.find(from);
        ASSERT_NE(at, std::string::npos) << from;
        body.replace(at, from.size(), to);
    };
    replace(R"("bid_client_id":149477874275629)", R"("bid_client_id":)" + std::to_string(client_index));
    return body;
}

TEST(LighterAccount, SendVerdictsFromBothTransports) {
    const auto ws_body = fixture("ws_send_tx_invalid_signature.json");
    const auto ws = parse_send_reply(ws_body);
    ASSERT_TRUE(ws.has_value());
    EXPECT_EQ(ws->id, "43");
    EXPECT_EQ(ws->code, 21120);
    EXPECT_EQ(ws->message, "invalid signature");
    EXPECT_EQ(classify_code(ws->code), ErrorClass::AuthFailure);

    const auto rest_body = fixture("rest_send_tx_api_key_not_found.json");
    const auto rest = parse_send_reply(rest_body);
    ASSERT_TRUE(rest.has_value());
    EXPECT_TRUE(rest->id.empty());
    EXPECT_EQ(rest->code, 21109);
    EXPECT_EQ(classify_code(rest->code), ErrorClass::AuthFailure);

    const auto ok = parse_send_reply(
        R"({"code":200,"message":"{\"ratelimit\": \"didn't use volume quota\"}","tx_hash":"ab12","predicted_execution_time_ms":1791726881683})");
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->ok());
    EXPECT_EQ(ok->tx_hash, "ab12");

    EXPECT_EQ(classify_code(k_code_invalid_nonce), ErrorClass::Retryable);
    EXPECT_EQ(classify_code(23000), ErrorClass::RateLimited);
    EXPECT_EQ(classify_code(21739), ErrorClass::Terminal);
}

TEST(LighterAccount, NextNonceAndAnExpiredTokenOnTheStream) {
    const auto nonce = parse_next_nonce(fixture("rest_next_nonce.json"));
    ASSERT_TRUE(nonce.has_value());
    EXPECT_EQ(*nonce, 1);
    EXPECT_FALSE(parse_next_nonce(fixture("rest_send_tx_api_key_not_found.json")).has_value());

    const auto error = parse_stream_error(fixture("ws_account_auth_expired.json"));
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, 20013);
    EXPECT_EQ(classify_code(error->code), ErrorClass::AuthFailure);
}

TEST(LighterAccount, AccountRowCarriesCollateralAndSignedPositions) {
    WireAccount account{};
    const auto body = fixture("rest_account.json");
    ASSERT_TRUE(parse_account(body, account).has_value());
    EXPECT_FALSE(account.collateral.empty());
    EXPECT_FALSE(account.available_balance.empty());
    ASSERT_EQ(account.positions.size(), 2U);
    EXPECT_EQ(account.positions[0].market_id, 0U);
    EXPECT_EQ(account.positions[0].sign, -1);
    EXPECT_EQ(account.positions[1].market_id, 1U);
    EXPECT_EQ(account.positions[1].sign, 1);
    EXPECT_FALSE(account.positions[1].avg_entry_price.empty());
}

TEST(LighterAccount, TradesFromRestAndTheStreamSnapshot) {
    std::vector<WireTrade> trades;
    const auto body = fixture("rest_trades.json");
    ASSERT_TRUE(parse_account_trades(body, trades).has_value());
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].trade_id, 34113211121U);
    EXPECT_EQ(trades[0].market_id, 1U);
    EXPECT_EQ(trades[0].size, "0.00020");
    EXPECT_EQ(trades[0].price, "83026.3");
    EXPECT_EQ(trades[0].bid_account_id, 27927);
    EXPECT_FALSE(trades[0].is_maker_ask);
    const auto bid = side_of(trades[0], 27927);
    EXPECT_FALSE(bid.is_ask);
    EXPECT_TRUE(bid.maker);
    EXPECT_EQ(bid.client_index, 149477874275629);
    // a premium maker's 0.0038% on 16.605260 usdc
    EXPECT_EQ(fee_of(trades[0], true).raw, 630999);
    EXPECT_EQ(fee_of(trades[0], false).raw, 0);
    EXPECT_FALSE(side_of(trades[0], 737487).maker);

    const auto snapshot = fixture("ws_account_all_trades_subscribed.json");
    ASSERT_TRUE(parse_account_trades(snapshot, trades).has_value());
    EXPECT_TRUE(trades.empty());
}

TEST(LighterAccount, OrderStatusesMapToPhases) {
    EXPECT_EQ(order_phase("open"), OrderPhase::Live);
    EXPECT_EQ(order_phase("in-progress"), OrderPhase::Live);
    EXPECT_EQ(order_phase("filled"), OrderPhase::Filled);
    EXPECT_EQ(order_phase("canceled"), OrderPhase::Canceled);
    EXPECT_EQ(order_phase("canceled-not-enough-liquidity"), OrderPhase::Canceled);
    EXPECT_EQ(order_phase("canceled-post-only"), OrderPhase::PostOnlyCrossed);
    EXPECT_EQ(order_phase("mystery"), OrderPhase::Unknown);
}

TEST(LighterAccount, ClientOrderIndexIsTheUlidsLowBits) {
    const auto a = client_order_index(ulid("01K7AB3C4D5E6F7G8H9J0KMNPQ"));
    const auto b = client_order_index(ulid("01K7AB3C4D5E6F7G8H9J0KMNPR"));
    EXPECT_GT(a, 0);
    EXPECT_LT(a, std::int64_t{1} << 48);
    EXPECT_EQ(b, a + 1);
    EXPECT_EQ(client_order_index(ulid("01K7AB3C4D0000000000000000")), 1);
}

class LighterLifecycle : public ::testing::Test {
protected:
    exec::InstrumentTable table;
    InstrumentId btc{load_btc(table)};
    CaptureSink sink;
    Lifecycle orders{sink, table, k_account};
    std::vector<WireOrder> order_rows;
    std::vector<WireTrade> trade_rows;

    void push_orders(const std::string& frame) {
        ASSERT_TRUE(parse_account_orders(frame, order_rows).has_value());
        for (const auto& row : order_rows) {
            orders.on_order(row, k_now);
        }
    }

    void push_trades(const std::string& frame) {
        ASSERT_TRUE(parse_account_trades(frame, trade_rows).has_value());
        for (const auto& row : trade_rows) {
            orders.on_trade(row, k_now);
        }
    }
};

TEST_F(LighterLifecycle, PartialIocFillsThenExpires) {
    const auto cmd = ioc_buy(btc, 300);
    const auto index = client_order_index(cmd.exchange_client_order_id);
    orders.track(cmd);
    push_trades(trades_frame(index));
    push_orders(orders_frame(index, "canceled-not-enough-liquidity", "0.00020"));

    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Accepted);
    EXPECT_EQ(sink.events[0].exchange_order_id.view(), "844421320500134");
    EXPECT_EQ(sink.events[1].type, OrderEventType::Filled);
    EXPECT_EQ(sink.events[1].qty.raw, 20);
    EXPECT_EQ(sink.events[1].price.raw, 830263);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 280);
    EXPECT_EQ(sink.events[1].liquidity, Liquidity::Maker);
    EXPECT_EQ(sink.events[1].fee.raw, 630999);
    EXPECT_EQ(sink.events[1].exec_id.view(), "34113211121");
    EXPECT_EQ(sink.events[2].type, OrderEventType::Expired);
    EXPECT_EQ(orders.tracked(), 0U);

    // a resent snapshot is not a second fill
    orders.track(cmd);
    push_trades(trades_frame(index));
    EXPECT_EQ(sink.events.size(), 3U);
}

TEST_F(LighterLifecycle, ACloseAheadOfItsTradeWaitsForIt) {
    const auto cmd = ioc_buy(btc, 300);
    const auto index = client_order_index(cmd.exchange_client_order_id);
    orders.track(cmd);
    push_orders(orders_frame(index, "canceled", "0.00020"));
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Accepted);
    push_trades(trades_frame(index));
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::Filled);
    EXPECT_EQ(sink.events[2].type, OrderEventType::Expired);
    EXPECT_EQ(orders.tracked(), 0U);
}

TEST_F(LighterLifecycle, AFullFillClosesWithoutACancel) {
    const auto cmd = ioc_buy(btc, 20);
    const auto index = client_order_index(cmd.exchange_client_order_id);
    orders.track(cmd);
    push_orders(orders_frame(index, "filled", "0.00020"));
    push_trades(trades_frame(index));
    ASSERT_EQ(sink.events.size(), 2U);
    EXPECT_EQ(sink.events[1].type, OrderEventType::Filled);
    EXPECT_EQ(sink.events[1].leaves_qty.raw, 0);
    EXPECT_EQ(orders.tracked(), 0U);
}

TEST_F(LighterLifecycle, ACrossingPostOnlyIsARejection) {
    auto cmd = ioc_buy(btc, 300);
    cmd.tif = TimeInForce::Gtc;
    cmd.flags = OrderFlags::PostOnly;
    const auto index = client_order_index(cmd.exchange_client_order_id);
    orders.track(cmd);
    push_orders(orders_frame(index, "canceled-post-only", "0.00000"));
    ASSERT_EQ(sink.events.size(), 1U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.errors.size(), 1U);
    EXPECT_EQ(orders.tracked(), 0U);
}

TEST_F(LighterLifecycle, AnOrderThisGatewayDidNotSendIsIgnored) {
    push_orders(orders_frame(12345, "open", "0.00000"));
    push_trades(trades_frame(12345));
    EXPECT_TRUE(sink.events.empty());
}

} // namespace
} // namespace raikiri::venues::lighter
