#include <raikiri/common/time.hpp>
#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/edgex/edgex_venue.hpp>
#include <raikiri/venues/edgex/trade.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace raikiri::venues::edgex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string hex(const evm::Hash& value) {
    std::string out{"0x"};
    for (const auto byte : value) {
        constexpr std::string_view digits = "0123456789abcdef";
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

// python's json.dumps puts a space after every separator; none of these values holds one
std::string compact(std::string text) {
    for (const std::string_view from : {", ", ": "}) {
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
            text.erase(at + 1, 1);
        }
    }
    return text;
}

// every vector below is edgex-python-sdk 2.0.1 output for these inputs: Client.create_order and
// AsyncClient.make_authenticated_request with the clock pinned and the session recording, the
// trading key being hardhat's public test key #0
constexpr std::uint64_t k_now_ms = 1791720000000ULL;
constexpr std::uint64_t k_account = 724625476626153743ULL;
constexpr std::string_view k_trading_key = "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";

struct OrderVector {
    OrderTerms terms;
    std::string_view nonce;
    std::string_view digest;
    std::string_view signature;
    std::string_view hmac;
    std::string_view body;
};

class EdgexSigning : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(
            builder_.load(k_account, "kat-api-key-0001", "kat-passphrase-0001", "kat-api-secret-0001", k_trading_key));
        auto meta = parse_signing_meta(fixture("rest_meta_perp.json"));
        ASSERT_TRUE(meta.has_value());
        builder_.set_meta(std::move(*meta));
    }

    RequestBuilder builder_;
};

TEST_F(EdgexSigning, TheMetadataNamesTheDomainAndTheContract) {
    EXPECT_EQ(builder_.signer(), "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266");
    const auto& meta = builder_.meta();
    EXPECT_EQ(meta.chain_id, 3343U); // nativeChainId wins over chainId 0xd0f, the same number
    EXPECT_EQ(hex(domain_separator(meta)), "0x79de539348476909462cecff9279ca69c43987ba60e1590baced93d936a211d6");
    const auto btc = meta.contracts.find(30000001);
    ASSERT_NE(btc, meta.contracts.end());
    EXPECT_EQ(btc->second.resolution, 10'000'000'000ULL);
    EXPECT_EQ(btc->second.collateral_id, 1000U);
    EXPECT_EQ(btc->second.collateral_resolution, 1'000'000U);
    // max(0.00045, 0.0004)
    EXPECT_EQ(static_cast<std::uint64_t>(btc->second.fee_rate.mantissa), 45U);
    EXPECT_EQ(btc->second.fee_rate.scale, 5U);
}

TEST(EdgexNonce, IsTheFirst32BitsOfTheClientIdsSha256) {
    EXPECT_EQ(l2_nonce("kat-buy-0001"), 2282593784U);
    EXPECT_EQ(l2_nonce("kat-sell-0002"), 1918773737U);
    EXPECT_EQ(l2_nonce("kat-sell-0003"), 944839142U);
}

TEST(EdgexDecimal, KeepsPythonsTrailingZeros) {
    std::array<char, 48> out{};
    const auto product = [&](std::string_view a, std::string_view b) {
        const auto x = parse_decimal(a);
        const auto y = parse_decimal(b);
        return std::string{format_decimal(
            Decimal{.mantissa = x->mantissa * y->mantissa, .scale = static_cast<std::uint8_t>(x->scale + y->scale)},
            out)};
    };
    EXPECT_EQ(product("97463.4", "0.001"), "97.4634");
    EXPECT_EQ(product("95000.0", "0.05"), "4750.000");
    EXPECT_EQ(product("101234.5", "0.123"), "12451.8435");
    EXPECT_EQ(product("2", "3"), "6");
    EXPECT_EQ(product("0.5", "0.01"), "0.005");
    EXPECT_FALSE(parse_decimal("-1").has_value());
    EXPECT_FALSE(parse_decimal("1.2.3").has_value());
    EXPECT_FALSE(parse_decimal("").has_value());
}

TEST_F(EdgexSigning, OrdersSignAndSerialiseExactlyAsTheSdk) {
    const std::array<OrderVector, 3> vectors{{
        {.terms = {.account_id = k_account,
                   .contract_id = 30000001,
                   .side = Side::Buy,
                   .price = "97463.4",
                   .size = "0.001",
                   .time_in_force = "GOOD_TIL_CANCEL",
                   .client_order_id = "kat-buy-0001",
                   .now_ms = k_now_ms},
         .nonce = "2282593784",
         .digest = "0x980c8de5a22d6552018929b751200446a2a82c4a784ef0d6512bfc5df0faa9a2",
         .signature = "c731535c7e9291e8f9ff05922d614acfab85e8eab369e1711a8bb20e81cb95232725e03bf81b4de50c5faac4eb06dc47"
                      "af"
                      "7166b20f932cbb7c2e2f3d1d9d79d31c",
         .hmac = "34fabe6bc49926327d55e3162829d357a5bf04bdeeff9a10f309b3d3fe3c0b74",
         .body =
             R"({"accountId": "724625476626153743", "contractId": "30000001", "price": "97463.4", "size": "0.001", "type": "LIMIT", "side": "BUY", "timeInForce": "GOOD_TIL_CANCEL", "clientOrderId": "kat-buy-0001", "expireTime": "1793620800000", "l2Nonce": "2282593784", "l2Signature": "c731535c7e9291e8f9ff05922d614acfab85e8eab369e1711a8bb20e81cb95232725e03bf81b4de50c5faac4eb06dc47af7166b20f932cbb7c2e2f3d1d9d79d31c", "l2ExpireTime": "1794312000000", "l2Value": "97.4634", "l2Size": "0.001", "l2LimitFee": "1", "reduceOnly": false, "triggerPrice": "", "triggerPriceType": "", "sourceKey": "", "isPositionTpsl": false, "openTpslParentOrderId": "", "isSetOpenTp": false, "openTp": null, "isSetOpenSl": false, "openSl": null, "extraType": "", "extraDataJson": ""})"},
        {.terms = {.account_id = k_account,
                   .contract_id = 30000001,
                   .side = Side::Sell,
                   .price = "101234.5",
                   .size = "0.123",
                   .time_in_force = "POST_ONLY",
                   .client_order_id = "kat-sell-0002",
                   .now_ms = k_now_ms},
         .nonce = "1918773737",
         .digest = "0xa03da17dc8e1e91137be0abe6a2ecea7033e25c5b46a5edfaf6547a7dac61233",
         .signature = "89c9ae37ef9bf2f09aa93b077eb0b9dc467a215e5740607908b62782f57fe7a80600f2d5c81e4efb9161866db9e5cd1e"
                      "cd"
                      "e3a11ddf6174a5be532e32a7cda5671c",
         .hmac = "6d6c46deaf9e23df912d3285b8da86ba2fe17f8570d8ecdd2320884bbddda44c",
         .body =
             R"({"accountId": "724625476626153743", "contractId": "30000001", "price": "101234.5", "size": "0.123", "type": "LIMIT", "side": "SELL", "timeInForce": "POST_ONLY", "clientOrderId": "kat-sell-0002", "expireTime": "1793620800000", "l2Nonce": "1918773737", "l2Signature": "89c9ae37ef9bf2f09aa93b077eb0b9dc467a215e5740607908b62782f57fe7a80600f2d5c81e4efb9161866db9e5cd1ecde3a11ddf6174a5be532e32a7cda5671c", "l2ExpireTime": "1794312000000", "l2Value": "12451.8435", "l2Size": "0.123", "l2LimitFee": "6", "reduceOnly": false, "triggerPrice": "", "triggerPriceType": "", "sourceKey": "", "isPositionTpsl": false, "openTpslParentOrderId": "", "isSetOpenTp": false, "openTp": null, "isSetOpenSl": false, "openSl": null, "extraType": "", "extraDataJson": ""})"},
        {.terms = {.account_id = k_account,
                   .contract_id = 30000001,
                   .side = Side::Sell,
                   .price = "95000.0",
                   .size = "0.05",
                   .time_in_force = "IMMEDIATE_OR_CANCEL",
                   .client_order_id = "kat-sell-0003",
                   .reduce_only = true,
                   .now_ms = k_now_ms,
                   .expire_ms = 1792324800000ULL},
         .nonce = "944839142",
         .digest = "0x5b5a7a9eed8b8add1808e79f8da7f1b47576458d26af83850baeee883fbdf1cc",
         .signature = "c3a9010c28b0ac691f38245b57e2f1a321c51b1520e2df3882cc54c54005263e512b60f8bd37937843c43cbc84e6f59b"
                      "db"
                      "3624c22520daee94681256387420a81b",
         .hmac = "e090855bde84a3250320a913fd13ad28dca019883d942ea8f01440b5a3366e1d",
         .body =
             R"({"accountId": "724625476626153743", "contractId": "30000001", "price": "95000.0", "size": "0.05", "type": "LIMIT", "side": "SELL", "timeInForce": "IMMEDIATE_OR_CANCEL", "clientOrderId": "kat-sell-0003", "expireTime": "1792324800000", "l2Nonce": "944839142", "l2Signature": "c3a9010c28b0ac691f38245b57e2f1a321c51b1520e2df3882cc54c54005263e512b60f8bd37937843c43cbc84e6f59bdb3624c22520daee94681256387420a81b", "l2ExpireTime": "1793016000000", "l2Value": "4750.000", "l2Size": "0.05", "l2LimitFee": "3", "reduceOnly": true, "triggerPrice": "", "triggerPriceType": "", "sourceKey": "", "isPositionTpsl": false, "openTpslParentOrderId": "", "isSetOpenTp": false, "openTp": null, "isSetOpenSl": false, "openSl": null, "extraType": "", "extraDataJson": ""})"},
    }};
    std::array<std::uint8_t, 20> signer{};
    ASSERT_TRUE(evm::from_hex(builder_.signer(), signer));
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        const auto& v = vectors[i];
        const auto& contract = builder_.meta().contracts.at(30000001);
        const auto order = l2_order(v.terms, contract);
        ASSERT_TRUE(order.has_value()) << i;
        EXPECT_EQ(std::to_string(order->nonce), v.nonce) << i;
        EXPECT_EQ(hex(order_digest(domain_separator(builder_.meta()), v.terms, contract, *order, signer)), v.digest)
            << i;

        const auto request = builder_.create_order(v.terms);
        ASSERT_TRUE(request.has_value()) << i;
        EXPECT_EQ(builder_.last_signature(), v.signature) << i;
        EXPECT_EQ(request->body, compact(std::string{v.body})) << i;
        EXPECT_EQ(request->target, "/api/v2/private/order/createOrder");
        ASSERT_EQ(request->headers.size(), 4U);
        EXPECT_EQ(request->headers[0].value, "kat-api-key-0001");
        EXPECT_EQ(request->headers[1].value, "kat-passphrase-0001");
        EXPECT_EQ(request->headers[2].value, v.hmac) << i << ' ' << builder_.last_signed_message();
        EXPECT_EQ(request->headers[3].value, "1791720000000");
    }
}

// the first order's amounts, as the sdk's typed-data message states them
TEST_F(EdgexSigning, AmountsTruncateAndTheFeeLimitRoundsUpToWholeCollateral) {
    const OrderTerms terms{.account_id = k_account,
                           .contract_id = 30000001,
                           .side = Side::Buy,
                           .price = "97463.4",
                           .size = "0.001",
                           .client_order_id = "kat-buy-0001",
                           .now_ms = k_now_ms};
    const auto order = l2_order(terms, builder_.meta().contracts.at(30000001));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(order->amount_synthetic), 10'000'000U);
    EXPECT_EQ(static_cast<std::uint64_t>(order->amount_collateral), 97'463'400U);
    EXPECT_EQ(static_cast<std::uint64_t>(order->amount_fee), 1'000'000U);
    EXPECT_EQ(order->limit_fee, 1U);
    EXPECT_EQ(order->expire_ms, 1793620800000ULL); // signed as seconds
    EXPECT_EQ(order->l2_expire_ms, 1794312000000ULL);
}

TEST_F(EdgexSigning, ReadsAndCancelsSignTheSortedParameters) {
    const std::array<Param, 3> active{
        {{"accountId", "724625476626153743"}, {"filterContractIdList", "30000001"}, {"size", "50"}}};
    const auto get = builder_.get(k_active_orders, active, k_now_ms);
    ASSERT_TRUE(get.has_value());
    EXPECT_EQ(builder_.last_signed_message(),
              "1791720000000GET/api/v2/private/order/getActiveOrderPage"
              "accountId=724625476626153743&filterContractIdList=30000001&size=50");
    EXPECT_EQ(get->headers[2].value, "e54dc5a6aaf31d022f648416850f6c483cb232714996cef81736a0a8fe39514d");
    EXPECT_EQ(
        get->target,
        "/api/v2/private/order/getActiveOrderPage?accountId=724625476626153743&filterContractIdList=30000001&size=50");
    EXPECT_EQ(get->method, net::HttpMethod::Get);

    const std::array<Param, 2> by_client{
        {{"accountId", "724625476626153743"}, {"clientOrderIdList", "kat-buy-0001,kat-sell-0002"}}};
    const auto query = builder_.get(k_order_by_client_id, by_client, k_now_ms);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query->headers[2].value, "2231ec810a29bcf68df2dd2dfc93f3650b0fefe166d5cc2e56091126a0d54dd8");

    const auto cancel = builder_.cancel_order("kat-buy-0001", k_now_ms);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(cancel->body, compact(R"({"accountId": "724625476626153743", "clientOrderIdList": ["kat-buy-0001"]})"));
    EXPECT_EQ(cancel->headers[2].value, "bf786e9c1f22d6903ba080a3ea8a9a3ff1f93ba7cb7c3525dd29800e0099be03");
}

TEST_F(EdgexSigning, ThePrivateSocketSignsItsQueryOnTheHandshake) {
    const auto headers = builder_.stream_headers(k_now_ms);
    ASSERT_EQ(headers.size(), 4U);
    EXPECT_EQ(builder_.last_signed_message(),
              "1791720000000GET/api/v1/private/wsaccountId=724625476626153743&timestamp=1791720000000");
    EXPECT_EQ(headers[3].second, "1791720000000");
    EXPECT_EQ(builder_.stream_target(), "/api/v1/private/ws?accountId=724625476626153743&timestamp=1791720000000");
}

TEST(EdgexSigningRefusals, NothingIsBuiltWithoutCredentialsOrMetadata) {
    RequestBuilder empty;
    const OrderTerms terms{.contract_id = 30000001, .price = "1", .size = "1", .client_order_id = "x"};
    EXPECT_EQ(empty.create_order(terms).error(), BuildError::NotSigned);
    RequestBuilder keyed;
    ASSERT_TRUE(keyed.load(k_account, "k", "p", "s", k_trading_key));
    EXPECT_EQ(keyed.create_order(terms).error(), BuildError::UnknownContract);
    EXPECT_FALSE(keyed.load(0, "k", "p", "s", k_trading_key));
    EXPECT_FALSE(keyed.load(k_account, "k", "p", "s", "0xnot-a-key"));
}

TEST(EdgexReply, UnauthenticatedRefusalsAreAuthFailures) {
    const auto missing_body = fixture("rest_create_no_headers.json");
    const auto missing = parse_reply(missing_body);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, "GATEWAY_HEADER_REQUIRED");
    EXPECT_EQ(classify(400, missing->code), ErrorClass::AuthFailure);
    const auto bad_key_body = fixture("rest_create_bad_key.json");
    const auto bad_key = parse_reply(bad_key_body);
    ASSERT_TRUE(bad_key.has_value());
    EXPECT_EQ(bad_key->code, "INVALID_API_KEY");
    EXPECT_EQ(bad_key->msg, "invalid apiKey : x");
    EXPECT_EQ(classify(401, bad_key->code), ErrorClass::AuthFailure);
    EXPECT_EQ(classify(429, ""), ErrorClass::RateLimited);
    EXPECT_EQ(classify(200, "INSUFFICIENT_BALANCE"), ErrorClass::Terminal);
}

TEST(EdgexReply, ACreateNamesItsOrderAndACancelItsResult) {
    const auto created = parse_reply(R"({"code":"SUCCESS","data":{"orderId":"564827797948727434"},"msg":null})");
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->code, "SUCCESS");
    EXPECT_EQ(created->order_id, "564827797948727434");
    const auto canceled = parse_reply(
        R"({"code":"SUCCESS","data":{"cancelResultMap":{"564827797948727434":"ORDER_NOT_FOUND"}}})");
    ASSERT_TRUE(canceled.has_value());
    EXPECT_EQ(canceled->cancel_result, "ORDER_NOT_FOUND");
    EXPECT_TRUE(is_benign_cancel_failure(canceled->cancel_result));
}

InstrumentBase eth_base() {
    InstrumentBase base{};
    base.tick_size = FixedE9::from_raw(10'000'000); // 0.01
    base.lot_size = FixedE9::from_raw(10'000'000);  // 0.01
    return base;
}

TEST(EdgexPrivateStream, AnOrderUpdateCarriesTheOrderAndItsFills) {
    const auto frame = fixture("ws_private_order_update.json");
    const auto kind = parse_private_kind(frame);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(kind->kind, PrivateKind::Trade);
    EXPECT_EQ(kind->event, "ORDER_UPDATE");

    TradeBuffer storage;
    const auto rows = parse_trade_event(frame, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->orders.size(), 1U);
    ASSERT_EQ(rows->fills.size(), 1U);
    ASSERT_EQ(rows->positions.size(), 1U);
    EXPECT_EQ(rows->orders[0].status, "FILLED");
    EXPECT_EQ(rows->orders[0].cum_match_size, "0.02");
    EXPECT_EQ(rows->orders[0].contract_id, 10000002U);
    const auto& fill = rows->fills[0];
    EXPECT_EQ(fill.order_id, "729107150223180559");
    EXPECT_EQ(fill.contract_id, 0U); // the push leaves it to the order row

    // a filled order is told by its fills, not its row
    EXPECT_FALSE(map_order(rows->orders[0], eth_base(), UnixNanos{1}).has_value());

    auto complete = fill;
    complete.side = rows->orders[0].side;
    const auto event = map_fill(complete, eth_base(), QtyLots::from_raw(0), UnixNanos{1});
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->type, OrderEventType::Filled);
    EXPECT_EQ(event->side, Side::Sell);
    EXPECT_EQ(event->price.raw, 230976);
    EXPECT_EQ(event->qty.raw, 2);
    EXPECT_EQ(event->fee.raw, 17'554'000);
    EXPECT_EQ(event->liquidity, Liquidity::Taker);
    EXPECT_EQ(event->exec_id.view(), "729108951626416911");
    EXPECT_EQ(event->ts_event.ns, 1773833120252ULL * 1'000'000ULL);

    const auto position = map_position(rows->positions[0], eth_base());
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->net_qty.raw, 1);
}

TEST(EdgexPrivateStream, ConnectedAndPingAreTold) {
    const auto connected = parse_private_kind(fixture("ws_private_connected.json"));
    ASSERT_TRUE(connected.has_value());
    EXPECT_EQ(connected->kind, PrivateKind::Connected);
    const auto ping = parse_private_kind(R"({"type":"ping","time":"1773833240000"})");
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->kind, PrivateKind::Ping);
}

TEST(EdgexOrderRows, OpenIsAcceptedAndCanceledIsCanceled) {
    WireOrder open{.id = "1",
                   .client_order_id = "01JABCDEFGHJKMNPQRSTVWXYZ0",
                   .contract_id = 10000002,
                   .side = "BUY",
                   .price = "2300.10",
                   .size = "0.05",
                   .status = "OPEN",
                   .cum_match_size = "0",
                   .updated_ms = 5};
    const auto accepted = map_order(open, eth_base(), UnixNanos{1});
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->leaves_qty.raw, 5);
    EXPECT_EQ(accepted->price.raw, 230010);
    EXPECT_EQ(accepted->exchange_client_order_id.view(), "01JABCDEFGHJKMNPQRSTVWXYZ0");

    auto partly = open;
    partly.cum_match_size = "0.02";
    EXPECT_FALSE(map_order(partly, eth_base(), UnixNanos{1}).has_value());
    const auto report = map_order_report(partly, eth_base());
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report->leaves.raw, 3);

    auto canceled = open;
    canceled.status = "CANCELED";
    const auto gone = map_order(canceled, eth_base(), UnixNanos{1});
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone->type, OrderEventType::Canceled);
    EXPECT_EQ(gone->leaves_qty.raw, 0);
}

TEST(EdgexOrderRows, TheRestPagesReadAsOrdersOrFills) {
    TradeBuffer storage;
    const auto orders = parse_rest_rows(
        R"({"code":"SUCCESS","data":[{"id":"564829588270612618","contractId":"10000001","side":"BUY","price":"96260.7","size":"0.001","clientOrderId":"9311381563209122","status":"OPEN","cumFillSize":"0","updatedTime":"1734665867876"}]})",
        storage,
        false);
    ASSERT_TRUE(orders.has_value());
    ASSERT_EQ(orders->orders.size(), 1U);
    EXPECT_EQ(orders->orders[0].cum_match_size, "0");
    const auto fills = parse_rest_rows(
        R"({"code":"SUCCESS","data":{"dataList":[{"id":"9","orderId":"8","contractId":"10000001","orderSide":"SELL","fillSize":"0.001","fillPrice":"97000.1","fillFee":"0.04","direction":"MAKER","matchTime":"1"}],"nextPageOffsetData":""}})",
        storage,
        true);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->fills.size(), 1U);
    EXPECT_EQ(fills->fills[0].side, "SELL");
    EXPECT_EQ(fills->fills[0].contract_id, 10000001U);
    EXPECT_FALSE(parse_rest_rows(R"({"code":"INVALID_API_KEY","data":null})", storage, false).has_value());
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

TEST(EdgexVenue, CredentialsPackFiveValuesIntoThreeReferences) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    const std::string secret = "literal:kat-api-secret-0001 " + std::string{k_trading_key};
    const auto good = exec::make_venue({.venue = VenueId::Edgex,
                                        .market = MarketType::Perp,
                                        .api_key_env = "literal:724625476626153743:kat-api-key-0001",
                                        .api_secret_env = secret,
                                        .passphrase_env = "literal:kat-passphrase-0001"},
                                       sink,
                                       clock);
    ASSERT_TRUE(good.has_value());
    EXPECT_TRUE(good->credential_errors.empty());
    EXPECT_TRUE(good->caps.cancel_by_client_order_id);

    const auto unsplit = exec::make_venue({.venue = VenueId::Edgex,
                                           .market = MarketType::Perp,
                                           .api_key_env = "literal:kat-api-key-0001",
                                           .api_secret_env = "literal:kat-api-secret-0001",
                                           .passphrase_env = "literal:kat-passphrase-0001"},
                                          sink,
                                          clock);
    ASSERT_TRUE(unsplit.has_value());
    ASSERT_EQ(unsplit->credential_errors.size(), 1U);
}

TEST(EdgexVenue, WithoutKeysEveryOrderIsRefusedThroughTheSink) {
    register_adapter();
    CaptureSink sink;
    const LiveClock clock;
    auto adapter = exec::make_venue({.venue = VenueId::Edgex, .market = MarketType::Perp}, sink, clock);
    ASSERT_TRUE(adapter.has_value());
    ASSERT_TRUE(adapter->execution->start().has_value());
    EXPECT_FALSE(adapter->execution->connected());
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.client_order_id = ClientOrderId::make(1, 42);
    cmd.qty = QtyLots::from_raw(5);
    adapter->execution->submit(cmd);
    adapter->execution->cancel(cmd);
    adapter->execution->amend(cmd);
    ASSERT_EQ(sink.events.size(), 3U);
    EXPECT_EQ(sink.events[0].type, OrderEventType::Rejected);
    EXPECT_EQ(sink.events[1].type, OrderEventType::CancelRejected);
    EXPECT_EQ(sink.events[2].type, OrderEventType::ModifyRejected);
    EXPECT_FALSE(adapter->execution->open_order_reports(MarketType::Perp).has_value());
}

} // namespace
} // namespace raikiri::venues::edgex
