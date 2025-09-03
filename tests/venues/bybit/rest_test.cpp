#include <raikiri/venues/bybit/map.hpp>
#include <raikiri/venues/bybit/rest.hpp>

#include <gtest/gtest.h>

#include <string>

namespace raikiri::venues::bybit {
namespace {

constexpr std::string_view k_api_key = "TESTKEY123";
constexpr std::string_view k_secret = "testsecret";
constexpr UnixNanos k_now{1'700'000'000'123'000'000ULL}; // 1700000000123 ms

exec::InstrumentTable make_table() {
    exec::InstrumentTable table;
    table.reserve(2);
    WireInstrument btc{};
    btc.base_coin = "BTC";
    btc.quote_coin = "USDT";
    btc.symbol = "BTCUSDT";
    btc.status = "Trading";
    btc.tick_size = "0.10";
    btc.qty_step = "0.001";
    const auto instrument = map_instrument(btc, MarketType::Perp);
    EXPECT_TRUE(instrument.has_value());
    EXPECT_TRUE(table.add(*instrument).has_value());
    return table;
}

void load_credentials(net::Signer& signer) {
    signer.set_api_key(k_api_key);
    ASSERT_TRUE(signer.secret().assign(k_secret).has_value());
}

std::string_view header(const net::RestRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return {};
}

OrderCmd make_order(const exec::InstrumentTable& table) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.instrument = base_of(*table.find(std::string_view{"BTCUSDT"})).id;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.price = PriceTicks::from_raw(160'000); // 16000.0 at a 0.1 tick
    cmd.qty = QtyLots::from_raw(3);            // 0.003 at a 0.001 lot
    cmd.exchange_client_order_id = *ExchangeClientOrderId::from("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    return cmd;
}

TEST(BybitRestBuilder, CreateOrderBodyAndHeaders) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer, 5'000};

    const auto request = builder.create_order(make_order(table), table, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    EXPECT_EQ(request->method, net::HttpMethod::Post);
    EXPECT_EQ(request->target, "/v5/order/create");
    EXPECT_EQ(request->body,
              R"({"category":"linear","symbol":"BTCUSDT","side":"Buy","orderType":"Limit",)"
              R"("qty":"0.003","price":"16000","timeInForce":"GTC",)"
              R"("orderLinkId":"01ARZ3NDEKTSV4RRFFQ69G5FAV"})");

    EXPECT_EQ(header(*request, "X-BAPI-API-KEY"), k_api_key);
    EXPECT_EQ(header(*request, "X-BAPI-TIMESTAMP"), "1700000000123");
    EXPECT_EQ(header(*request, "X-BAPI-RECV-WINDOW"), "5000");
    EXPECT_EQ(header(*request, "X-BAPI-SIGN").size(), net::k_hex_bytes);
}

TEST(BybitRestBuilder, PostSignsTimestampKeyWindowAndBodyConcatenated) {
    // The exact bytes matter: bybit concatenates with no separators, and a
    // wrong composition surfaces as a generic auth error that reads like a
    // bad key.
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer, 5'000};

    const auto request = builder.create_order(make_order(table), table, k_now);
    ASSERT_TRUE(request.has_value());

    const std::string expected =
        std::string{"1700000000123"} + std::string{k_api_key} + "5000" + std::string{request->body};
    EXPECT_EQ(builder.last_sign_message(), expected);

    // And the digest is genuinely that message under that secret.
    std::array<char, net::k_hex_bytes> reference{};
    EXPECT_EQ(header(*request, "X-BAPI-SIGN"), net::hmac_sha256_hex(k_secret, expected, reference));
}

TEST(BybitRestBuilder, GetSignsTheQueryStringWithoutItsQuestionMark) {
    // The asymmetry with POST is the classic source of rejected signatures.
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer, 5'000};

    const auto request = builder.open_orders(MarketType::Perp, k_now);
    ASSERT_TRUE(request.has_value()) << to_string(request.error());

    EXPECT_EQ(request->method, net::HttpMethod::Get);
    EXPECT_EQ(request->target, "/v5/order/realtime?category=linear&settleCoin=USDT");
    EXPECT_TRUE(request->body.empty());

    const std::string expected = std::string{"1700000000123"} + std::string{k_api_key} + "5000" +
                                 "category=linear&settleCoin=USDT";
    EXPECT_EQ(builder.last_sign_message(), expected);
    EXPECT_EQ(builder.last_sign_message().find('?'), std::string_view::npos);
}

TEST(BybitRestBuilder, PostOnlyIsExpressedAsTimeInForce) {
    // Bybit spells post-only as a TIF value. Sending it as a boolean flag is
    // silently ignored and the order crosses.
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = make_order(table);
    cmd.flags = OrderFlags::PostOnly;
    const auto request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("timeInForce":"PostOnly")"), std::string_view::npos);
    EXPECT_EQ(request->body.find("postOnly"), std::string_view::npos);

    cmd.flags = OrderFlags::None;
    cmd.tif = TimeInForce::Ioc;
    EXPECT_NE(builder.create_order(cmd, table, k_now)->body.find(R"("timeInForce":"IOC")"),
              std::string_view::npos);
}

TEST(BybitRestBuilder, MarketOrderOmitsPriceAndReduceOnlyAppears) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = make_order(table);
    cmd.order_type = OrderType::Market;
    cmd.flags = OrderFlags::ReduceOnly;
    const auto request = builder.create_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->body.find(R"("price")"), std::string_view::npos)
        << "a market order carrying a price is a rejected order";
    EXPECT_NE(request->body.find(R"("reduceOnly":true)"), std::string_view::npos);
}

TEST(BybitRestBuilder, CancelPrefersTheVenueIdWhenBound) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = make_order(table);
    cmd.kind = CmdKind::Cancel;

    // Before the ack binds an exchange id, our own id is the only handle.
    auto request = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("orderLinkId":"01ARZ3NDEKTSV4RRFFQ69G5FAV")"),
              std::string_view::npos);
    EXPECT_EQ(request->target, "/v5/order/cancel");

    // Once bound, the venue id is the durable handle: bybit resolves
    // orderLinkId only within a window after creation.
    cmd.target_exchange_order_id = *ExchangeOrderId::from("1321003749386327552");
    request = builder.cancel_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("orderId":"1321003749386327552")"), std::string_view::npos);
    EXPECT_EQ(request->body.find("orderLinkId"), std::string_view::npos);
}

TEST(BybitRestBuilder, AmendOmitsFieldsThatWereNotSet) {
    // Bybit amends only what is present, so sending an unset price would
    // rewrite it to zero rather than leave it alone.
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = make_order(table);
    cmd.kind = CmdKind::Amend;
    cmd.target_exchange_order_id = *ExchangeOrderId::from("999");
    cmd.price = PriceTicks::undef(); // qty-only amend
    const auto request = builder.amend_order(cmd, table, k_now);
    ASSERT_TRUE(request.has_value());
    EXPECT_NE(request->body.find(R"("qty":"0.003")"), std::string_view::npos);
    EXPECT_EQ(request->body.find(R"("price")"), std::string_view::npos);
}

TEST(BybitRestBuilder, ReconciliationPullsAndCancelAll) {
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    const auto positions = builder.positions(MarketType::Perp, k_now);
    ASSERT_TRUE(positions.has_value());
    EXPECT_EQ(positions->target, "/v5/position/list?category=linear&settleCoin=USDT");

    const auto executions =
        builder.executions(MarketType::Perp, UnixNanos{1'700'000'000'000'000}, k_now);
    ASSERT_TRUE(executions.has_value());
    EXPECT_NE(executions->target.find("startTime=1700000000"), std::string_view::npos);

    // With no lower bound the parameter is omitted rather than sent as zero,
    // which bybit reads as "everything since the epoch" and rate-limits.
    const auto all_time = builder.executions(MarketType::Perp, UnixNanos{}, k_now);
    ASSERT_TRUE(all_time.has_value());
    EXPECT_EQ(all_time->target.find("startTime"), std::string_view::npos);

    // No category on the wallet pull: one unified account backs both markets,
    // and UNIFIED is the only accountType a unified key may name.
    const auto wallet = builder.wallet_balance(k_now);
    ASSERT_TRUE(wallet.has_value());
    EXPECT_EQ(wallet->target, "/v5/account/wallet-balance?accountType=UNIFIED");

    const auto cancel_all = builder.cancel_all(MarketType::Perp, k_now);
    ASSERT_TRUE(cancel_all.has_value());
    EXPECT_EQ(cancel_all->target, "/v5/order/cancel-all");
}

TEST(BybitRestBuilder, InstrumentCatalogueIsUnsigned) {
    // Public endpoint: attaching a signature would only risk leaking a stale
    // one to something that never asked for it.
    net::Signer signer;
    RequestBuilder builder{signer};
    const auto request = builder.instruments(MarketType::Perp);
    EXPECT_EQ(request.method, net::HttpMethod::Get);
    EXPECT_EQ(request.target, "/v5/market/instruments-info?category=linear&limit=1000");
    EXPECT_TRUE(request.headers.empty());
}

TEST(BybitRestBuilder, SpotUsesItsOwnCategory) {
    EXPECT_EQ(category_of(MarketType::Perp), "linear");
    EXPECT_EQ(category_of(MarketType::Spot), "spot");
    net::Signer signer;
    RequestBuilder builder{signer};
    EXPECT_NE(builder.instruments(MarketType::Spot).target.find("category=spot"),
              std::string_view::npos);
}

TEST(BybitRestBuilder, MissingCredentialsRefuseToBuildSignedRequests) {
    const auto table = make_table();
    net::Signer signer; // never loaded
    RequestBuilder builder{signer};

    EXPECT_EQ(builder.create_order(make_order(table), table, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(builder.open_orders(MarketType::Perp, k_now).error(), BuildError::NotSigned);
    EXPECT_EQ(builder.wallet_balance(k_now).error(), BuildError::NotSigned);
}

TEST(BybitRestBuilder, UnknownInstrumentIsRefused) {
    const auto table = make_table();
    net::Signer signer;
    load_credentials(signer);
    RequestBuilder builder{signer};

    auto cmd = make_order(table);
    cmd.instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{99});
    EXPECT_EQ(builder.create_order(cmd, table, k_now).error(), BuildError::UnknownInstrument);
}

} // namespace
} // namespace raikiri::venues::bybit
