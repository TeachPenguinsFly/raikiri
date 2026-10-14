#include <raikiri/venues/apex/trade.hpp>
#include <raikiri/venues/apex/zk.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::apex {
namespace {

std::string fixture(std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" / name;
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << "missing fixture " << path;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// parsed rows view the text they came from, so every fixture outlives the test
std::string_view kept(std::string_view name) {
    static std::map<std::string, std::string, std::less<>> cache;
    const auto [it, inserted] = cache.try_emplace(std::string{name}, fixture(name));
    return it->second;
}

std::vector<std::uint8_t> unhex(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

uint128_t u128(std::string_view text) {
    uint128_t out = 0;
    for (const char c : text) {
        out = (out * 10) + static_cast<uint128_t>(c - '0');
    }
    return out;
}

FixedE9 e9(std::string_view text) {
    return *parse_decimal_e9(text);
}

std::string_view header(const net::RestRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return {};
}

// every vector below is apexomni 3.4.2 output: its bundled zklink_sdk signer (ContractBuilder, Contract.get_bytes,
// ZkLinkSigner.new_from_seed, sign_musig) and HttpPrivateSign.create_order_v3 with the clock pinned and the
// transport recording; seeds and api credentials are random test values
struct ZkVector {
    std::string_view seed;
    zk::ContractOrder order;
    std::string_view bytes;
    std::string_view public_key;
    std::string_view signature;
};

const std::array<ZkVector, 5> k_zk_vectors{{
    {"ba8b2cadc426ce7b978254a25b51865acb26ef8b6fcf4fe4716e453c0fb3d772b450f9a980caa095a06750c943c16c9133f6"
     "13b355371c611903daba0c5e4664ed",
     {.account_id = 7,
      .sub_account_id = 0,
      .slot_id = 123,
      .nonce = 456,
      .pair_id = 33,
      .size = u128("1000000"),
      .price = u128("25000000000"),
      .buy = true,
      .taker_fee_rate = 5,
      .maker_fee_rate = 2,
      .has_subsidy = false},
     "fe0000000700007b0001c821010001e848000000000000000000000005d21dba00020500",
     "8c257520832c4cbbd1edb5519fa62d7016a5d185cf09a576811e572513db141d",
     "c9f177b609bab7ac501cd22a4fa9b02d1dd3da37dba4e67cdb3d689ee869962494a7da24a928648669907dd96c81fe7d8c24"
     "8b39babf8f5687bf6be242f05000"},
    {"1ee56ff8611721f4e324d4ced6fce2bac2d1455c0e5fd8b0fa220e3402f24b84",
     {.account_id = 2333842507,
      .sub_account_id = 0,
      .slot_id = 61234,
      .nonce = 4000000000,
      .pair_id = 50001,
      .size = u128("100000000000000000000"),
      .price = u128("123400000000000000"),
      .buy = false,
      .taker_fee_rate = 5,
      .maker_fee_rate = 2,
      .has_subsidy = false},
     "fe8b1b9c4b00ef326b280051004a817c800a0000000000000001b667a56d488000020500",
     "e231f3caae57a78767741836db0e5676f64536e777d8270f50c1f386a3506f24",
     "8877ef953c191cf03c696cade4fcd612a50157f1e444cbf20c03419092b82e94ca0c9638003a4b2776e44aacbb2135248636"
     "13171fe286beb7eadcf045e89505"},
    {"8e0100fed544df165e8ab7c6bf7dbd19cc5b0143001cc2937af3b0043602d5be2a368d50b03ad0fd5f4800362518f1ba9496"
     "363d18b6365dfd88dd428326cbc89a",
     {.account_id = 3505225848,
      .sub_account_id = 0,
      .slot_id = 1,
      .nonce = 16777217,
      .pair_id = 50052,
      .size = u128("12345678901234567890123"),
      .price = u128("999999999999999999999999"),
      .buy = true,
      .taker_fee_rate = 6,
      .maker_fee_rate = 0,
      .has_subsidy = true},
     "fed0ed807800000100000184015bfb8386ac0000000000d3c21bcecceda0ffffff000601",
     "4d47a8be2ff215a772ab7f32afdbcd7b5deb03a9964132e25c127a778690e424",
     "b24e95d97182d10e45dfec92736ecd6460c57d0e79c66895b7098c2e80884e1bacdc8d74e1fc78d981461574be0785f91537"
     "71712074706484959d79e7516f02"},
    {"3724050574cf968478789c76aaceb3d30278dfbafe75ca4c4338d6c6cd4913cc",
     {.account_id = 1,
      .sub_account_id = 0,
      .slot_id = 65535,
      .nonce = 0,
      .pair_id = 255,
      .size = u128("34359738367"),
      .price = u128("1"),
      .buy = false,
      .taker_fee_rate = 255,
      .maker_fee_rate = 255,
      .has_subsidy = false},
     "fe0000000100ffff000000ff00ffffffffe0000000000000000000000000000001ffff00",
     "2434c314860ffd972c6d341e5e7594c4f9355a316a65594d0441cae65ab60b87",
     "92061fda1fa5840820b071a9798c7b7a6397e2986ceb4162cce28ffe33d1eba2a336dd59591471fad586d82e66d918fc4f69"
     "d6b7fcb167fca38cb2f47711c103"},
    {"c9657d5f5ecb9626fd6c6a7e497d7df3659c3b60f76f4f5864d91bd3397495abd4a22cefc535630cff27d95a52fe467d3cbe"
     "6f6112558d1e62820d00c8252bdc6b",
     {.account_id = 4294967294,
      .sub_account_id = 0,
      .slot_id = 2,
      .nonce = 3,
      .pair_id = 50020,
      .size = u128("343597383680"),
      .price = u128("100000000000000000000000000000000000"),
      .buy = true,
      .taker_fee_rate = 0,
      .maker_fee_rate = 1,
      .has_subsidy = false},
     "fefffffffe0000020000036401ffffffffe113426172c74d822b878fe800000000010000",
     "cb9c997dd8bf9812d0258c670dabc0e271df61605e525108726fa8a9683fc58c",
     "5bfb8dbb30b74dd2486477f6120cfa30c68a36ef85945148bbae4e99c7cb68976524ac36557802e9d69c7cc2b5d5da2cfb6a"
     "10785a24edf4cb3495843fb7ce00"},
}};

constexpr std::string_view k_api_key = "3f1a9c4e-0b7d-4e2a-9c51-7d2e8f6a1b03";
constexpr std::string_view k_api_secret = "kT9vQ2xLm4pR8sW1yZ6bN3cF5hJ7dG0a";
constexpr std::string_view k_passphrase = "Xy7_pq2Lm9Rt4Wv";
constexpr std::string_view
    k_seeds = "0x4fe5dd4db838ac5272d9b5194b3ef3765f2f12eda0958f325f1c8695cb9dc8d607ab33763c98a7bdcfead518b558849077"
              "2cec5228a16933d09f411d0a286f51f0";
constexpr std::uint64_t k_account_id = 584232029671915612ULL;

struct OrderVector {
    OrderTerms terms;
    std::uint64_t now_ms;
    std::string_view signature;
    std::string_view message;
    std::string_view hmac;
    std::string_view body;
};

const std::array<OrderVector, 4> k_order_vectors{{
    {{.symbol = "DOGE-USDT",
      .side = Side::Buy,
      .price = e9("0.1987"),
      .size = e9("100"),
      .time_in_force = "POST_ONLY",
      .client_id = "7351902841",
      .reduce_only = false,
      .now_s = 1760448000},
     1760448001234ULL,
     "9a54d49e4acdf58e19e734708168565fbbcfa390544120174d268326f5830f01cc419a29bdde4332b1e8f0ed922ef150f65e"
     "0d4806d79a770b4d425bc9c2e600",
     "1760448001234POST/api/v3/orderclientId=7351902841&expiration=1762867200000&isOpenTpslOrder=False&isP"
     "ositionTpsl=False&isSetOpenSl=False&isSetOpenTp=False&limitFee=0.009935&price=0.1987&reduceOnly=Fals"
     "e&side=BUY&signature=0x9a54d49e4acdf58e19e734708168565fbbcfa390544120174d268326f5830f01cc419a29bdde4"
     "332b1e8f0ed922ef150f65e0d4806d79a770b4d425bc9c2e600&size=100&slLimitFee=None&symbol=DOGE-USDT&timeIn"
     "Force=POST_ONLY&tpLimitFee=None&type=LIMIT",
     "j8kXv4qNRdpnLQhtYUo+0U35aaYzPDEa/sIq5IiBhX0=",
     "symbol=DOGE-USDT&side=BUY&type=LIMIT&timeInForce=POST_ONLY&size=100&price=0.1987&limitFee=0.009935&e"
     "xpiration=1762867200000&clientId=7351902841&signature=0x9a54d49e4acdf58e19e734708168565fbbcfa3905441"
     "20174d268326f5830f01cc419a29bdde4332b1e8f0ed922ef150f65e0d4806d79a770b4d425bc9c2e600&reduceOnly=Fals"
     "e&isPositionTpsl=False&isOpenTpslOrder=False&isSetOpenSl=False&isSetOpenTp=False&slLimitFee=None&tpL"
     "imitFee=None"},
    {{.symbol = "ZRO-USDT",
      .side = Side::Sell,
      .price = e9("1.873"),
      .size = e9("7"),
      .time_in_force = "GOOD_TIL_CANCEL",
      .client_id = "1700000000000000001",
      .reduce_only = false,
      .now_s = 1760448001},
     1760448002345ULL,
     "5715c263c8c21a26419bb4275a817bb9c691a759c9debd48de1bdbf642ef2807206f21cc6dd43d6502093ada27a83ca1feeb"
     "3dd6a99729e59dbaafc36b326602",
     "1760448002345POST/api/v3/orderclientId=1700000000000000001&expiration=1762867201000&isOpenTpslOrder="
     "False&isPositionTpsl=False&isSetOpenSl=False&isSetOpenTp=False&limitFee=0.006556&price=1.873&reduceO"
     "nly=False&side=SELL&signature=0x5715c263c8c21a26419bb4275a817bb9c691a759c9debd48de1bdbf642ef2807206f"
     "21cc6dd43d6502093ada27a83ca1feeb3dd6a99729e59dbaafc36b326602&size=7&slLimitFee=None&symbol=ZRO-USDT&"
     "timeInForce=GOOD_TIL_CANCEL&tpLimitFee=None&type=LIMIT",
     "9wDHT1gPoLyw35j4dOg7lSbU6E83UYXQ4A+txedDHEs=",
     "symbol=ZRO-USDT&side=SELL&type=LIMIT&timeInForce=GOOD_TIL_CANCEL&size=7&price=1.873&limitFee=0.00655"
     "6&expiration=1762867201000&clientId=1700000000000000001&signature=0x5715c263c8c21a26419bb4275a817bb9"
     "c691a759c9debd48de1bdbf642ef2807206f21cc6dd43d6502093ada27a83ca1feeb3dd6a99729e59dbaafc36b326602&red"
     "uceOnly=False&isPositionTpsl=False&isOpenTpslOrder=False&isSetOpenSl=False&isSetOpenTp=False&slLimit"
     "Fee=None&tpLimitFee=None"},
    {{.symbol = "WIF-USDT",
      .side = Side::Sell,
      .price = e9("0.5121"),
      .size = e9("25"),
      .time_in_force = "IMMEDIATE_OR_CANCEL",
      .client_id = "r1",
      .reduce_only = true,
      .now_s = 1760448002},
     1760448003456ULL,
     "a440dad6ee53871c55d448f731b955486dabc9fc4d79b8c2dfe1e41c61f5ac075a396e76a78ab3d945d3524ea90e4b8d3312"
     "3af31fd647ad994d2c0a0d3ed001",
     "1760448003456POST/api/v3/orderclientId=r1&expiration=1762867202000&isOpenTpslOrder=False&isPositionT"
     "psl=False&isSetOpenSl=False&isSetOpenTp=False&limitFee=0.006402&price=0.5121&reduceOnly=True&side=SE"
     "LL&signature=0xa440dad6ee53871c55d448f731b955486dabc9fc4d79b8c2dfe1e41c61f5ac075a396e76a78ab3d945d35"
     "24ea90e4b8d33123af31fd647ad994d2c0a0d3ed001&size=25&slLimitFee=None&symbol=WIF-USDT&timeInForce=IMME"
     "DIATE_OR_CANCEL&tpLimitFee=None&type=LIMIT",
     "RW4x9MorMK6cSFJgJcNXYHHMKCuY/ru+tVFs+/pz93g=",
     "symbol=WIF-USDT&side=SELL&type=LIMIT&timeInForce=IMMEDIATE_OR_CANCEL&size=25&price=0.5121&limitFee=0"
     ".006402&expiration=1762867202000&clientId=r1&signature=0xa440dad6ee53871c55d448f731b955486dabc9fc4d7"
     "9b8c2dfe1e41c61f5ac075a396e76a78ab3d945d3524ea90e4b8d33123af31fd647ad994d2c0a0d3ed001&reduceOnly=Tru"
     "e&isPositionTpsl=False&isOpenTpslOrder=False&isSetOpenSl=False&isSetOpenTp=False&slLimitFee=None&tpL"
     "imitFee=None"},
    {{.symbol = "ENA-USDT",
      .side = Side::Buy,
      .price = e9("0.4009"),
      .size = e9("13"),
      .time_in_force = "GOOD_TIL_CANCEL",
      .client_id = "98765432109876543210",
      .reduce_only = false,
      .now_s = 1760448003},
     1760448004567ULL,
     "7805e8681b494fd86af0d62e11f2f8d4de254e08a76670c7d7a2d4c863145c2bd93dfc37a8f92032a4236fcd865e38719f40"
     "bc8871fa8484e9a46298a1776b04",
     "1760448004567POST/api/v3/orderclientId=98765432109876543210&expiration=1762867203000&isOpenTpslOrder"
     "=False&isPositionTpsl=False&isSetOpenSl=False&isSetOpenTp=False&limitFee=0.002606&price=0.4009&reduc"
     "eOnly=False&side=BUY&signature=0x7805e8681b494fd86af0d62e11f2f8d4de254e08a76670c7d7a2d4c863145c2bd93"
     "dfc37a8f92032a4236fcd865e38719f40bc8871fa8484e9a46298a1776b04&size=13&slLimitFee=None&symbol=ENA-USD"
     "T&timeInForce=GOOD_TIL_CANCEL&tpLimitFee=None&type=LIMIT",
     "clX9GKabChSix0daWQyYmdF3KUzfpTL7YkLsULPRRSU=",
     "symbol=ENA-USDT&side=BUY&type=LIMIT&timeInForce=GOOD_TIL_CANCEL&size=13&price=0.4009&limitFee=0.0026"
     "06&expiration=1762867203000&clientId=98765432109876543210&signature=0x7805e8681b494fd86af0d62e11f2f8"
     "d4de254e08a76670c7d7a2d4c863145c2bd93dfc37a8f92032a4236fcd865e38719f40bc8871fa8484e9a46298a1776b04&r"
     "educeOnly=False&isPositionTpsl=False&isOpenTpslOrder=False&isSetOpenSl=False&isSetOpenTp=False&slLim"
     "itFee=None&tpLimitFee=None"},
}};

void load(RequestBuilder& builder) {
    ASSERT_TRUE(builder.load(k_api_key, k_passphrase, k_api_secret, k_seeds));
    auto meta = parse_signing_meta(kept("rest_symbols_signing.json"));
    ASSERT_TRUE(meta.has_value());
    builder.set_meta(std::move(*meta));
    builder.set_account({.account_id = k_account_id, .taker_fee_rate = e9("0.00050"), .maker_fee_rate = e9("0.00020")});
}

InstrumentBase eth_base() {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Apex, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from("ETH/USDT");
    base.exchange_symbol = *SymbolText::from("ETHUSDT");
    base.tick_size = e9("0.1");
    base.lot_size = e9("0.001");
    base.min_notional = e9("5");
    base.contract_value = e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

TEST(ApexZk, SignsContractsLikeTheSdk) {
    for (const auto& v : k_zk_vectors) {
        const auto bytes = zk::contract_bytes(v.order);
        EXPECT_EQ(hex(bytes), v.bytes);
        zk::Signer signer;
        ASSERT_TRUE(signer.load(unhex(v.seed)));
        EXPECT_EQ(hex(signer.public_key()), v.public_key);
        EXPECT_EQ(hex(signer.sign(bytes)), v.signature);
    }
}

TEST(ApexZk, RescueHashMatchesTheSignerCrate) {
    // zklink_sdk's own test_rescue_hash_tx_msg
    const std::array<std::uint8_t, 4> message{1, 2, 3, 4};
    const std::array<std::uint8_t, 32> expected{249, 154, 208, 123, 96,  89,  132, 235, 231, 63,  56,
                                                200, 153, 131, 27,  183, 128, 71,  26,  245, 208, 120,
                                                49,  246, 233, 72,  230, 84,  66,  150, 170, 27};
    EXPECT_EQ(zk::rescue_hash_tx_msg(message), expected);
}

TEST(ApexZk, SlotAndNonceFollowPythonDivision) {
    struct Case {
        std::string_view client_id;
        std::uint64_t slot;
        std::uint32_t nonce;
    };

    // the last two round up to the next integer before int() truncates, so floor division would differ
    constexpr std::array<Case, 7> cases{{{"7351902841", 3889226923ULL, 2566021366U},
                                         {"1700000000000000001", 3209145352ULL, 1682704211U},
                                         {"r1", 3598687368ULL, 3166654606U},
                                         {"98765432109876543210", 4040254897ULL, 2959210742U},
                                         {"0", 1665465671ULL, 3586179735U},
                                         {"1970091", 1793084398ULL, 4294967053U},
                                         {"7372059", 2161507297ULL, 4294967112U}}};
    for (const auto& c : cases) {
        const auto got = zk::slot_nonce(c.client_id);
        EXPECT_EQ(got.slot, c.slot) << c.client_id;
        EXPECT_EQ(got.nonce, c.nonce) << c.client_id;
    }
}

TEST(ApexZk, LoadsHexSeedsAndRefusesShortOnes) {
    zk::Signer from_hex;
    ASSERT_TRUE(from_hex.load_hex(k_seeds));
    zk::Signer from_bytes;
    ASSERT_TRUE(from_bytes.load(unhex(k_seeds.substr(2))));
    EXPECT_EQ(from_hex.public_key(), from_bytes.public_key());
    zk::Signer refused;
    EXPECT_FALSE(refused.load_hex("0x00112233"));
    EXPECT_FALSE(refused.load_hex("0xzz"));
    EXPECT_FALSE(refused.loaded());
}

TEST(ApexOrders, CreateMatchesTheSdk) {
    RequestBuilder builder;
    load(builder);
    for (const auto& v : k_order_vectors) {
        const auto request = builder.create_order(v.terms, v.now_ms);
        ASSERT_TRUE(request.has_value()) << v.terms.client_id;
        EXPECT_EQ(builder.last_signature(), std::string{"0x"} + std::string{v.signature});
        EXPECT_EQ(builder.last_signed_message(), v.message);
        EXPECT_EQ(header(*request, "APEX-SIGNATURE"), v.hmac);
        EXPECT_EQ(header(*request, "APEX-API-KEY"), k_api_key);
        EXPECT_EQ(header(*request, "APEX-PASSPHRASE"), k_passphrase);
        EXPECT_EQ(header(*request, "APEX-TIMESTAMP"), std::to_string(v.now_ms));
        EXPECT_EQ(request->target, k_create_order);
        EXPECT_EQ(request->body, v.body);
        EXPECT_EQ(request->content_type, "application/x-www-form-urlencoded");
    }
}

TEST(ApexOrders, L2OrderScalesAndRoundsLikeTheSdk) {
    const SigningContract doge{.cross_symbol = "DOGEUSDT", .l2_pair_id = 50020, .decimals = 18};
    const AccountTerms account{
        .account_id = k_account_id, .taker_fee_rate = e9("0.00050"), .maker_fee_rate = e9("0.0002")};
    const auto order = l2_order(k_order_vectors[0].terms, doge, account);
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->contract.account_id, k_account_id % 4294967295ULL);
    EXPECT_TRUE(order->contract.price == u128("198700000000000000"));
    EXPECT_TRUE(order->contract.size == u128("100000000000000000000"));
    EXPECT_EQ(order->contract.taker_fee_rate, 5);
    EXPECT_EQ(order->contract.maker_fee_rate, 2);
    EXPECT_EQ(order->limit_fee_e6, 9935U);
    EXPECT_EQ(order->expiration_ms, 1762867200000ULL);

    const AccountTerms odd{.account_id = 1, .taker_fee_rate = e9("0.000251"), .maker_fee_rate = e9("0")};
    const auto rounded = l2_order(k_order_vectors[0].terms, doge, odd);
    ASSERT_TRUE(rounded.has_value());
    EXPECT_EQ(rounded->contract.taker_fee_rate, 3);
    EXPECT_EQ(rounded->contract.maker_fee_rate, 0);
    EXPECT_EQ(rounded->limit_fee_e6, 4988U); // 0.00498737 rounded up

    const AccountTerms none{};
    EXPECT_EQ(l2_order(k_order_vectors[0].terms, doge, none).error(), BuildError::NoAccount);
    const AccountTerms huge{.account_id = 1, .taker_fee_rate = e9("0.03"), .maker_fee_rate = e9("0")};
    EXPECT_EQ(l2_order(k_order_vectors[0].terms, doge, huge).error(), BuildError::BadFeeRate);
}

TEST(ApexOrders, PrivateReadsCancelsAndLoginMatchTheSdk) {
    RequestBuilder builder;
    load(builder);
    const auto cancel = builder.cancel(k_cancel_by_client_id, "7351902841", 1760448004569ULL);
    ASSERT_TRUE(cancel.has_value());
    EXPECT_EQ(header(*cancel, "APEX-SIGNATURE"), "tiqEZ5qEY0ttT15K8m1vmrEXQM4/MN5FP6xrozAtvaQ=");
    EXPECT_EQ(cancel->body, "id=7351902841");
    EXPECT_EQ(cancel->target, k_cancel_by_client_id);

    const auto open = builder.get(k_open_orders, {}, 1760448004570ULL);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(header(*open, "APEX-SIGNATURE"), "R9muKASco1LxERLZ4DpDKAraqNUDE4jD1awOOUWc0L4=");
    EXPECT_EQ(open->method, net::HttpMethod::Get);

    const std::array<Param, 3> params{{{"limit", "100"}, {"page", "0"}, {"beginTimeInclusive", "1760440000000"}}};
    const auto fills = builder.get(k_fills, params, 1760448004571ULL);
    ASSERT_TRUE(fills.has_value());
    EXPECT_EQ(header(*fills, "APEX-SIGNATURE"), "UEN3rHOSElzTcBGzyfWD7iaVWVl1p78gpi7BAr+B3Jw=");
    EXPECT_EQ(fills->target, "/api/v3/fills?limit=100&page=0&beginTimeInclusive=1760440000000");

    EXPECT_EQ(builder.stream_target(1760448000999ULL), "/realtime_private?v=2&timestamp=1760448000999");
    const auto login = builder.login_frame();
    EXPECT_NE(login.find(R"(\"signature\":\"tNBetApqMxTgNwgOd3TGoXBVZPTuY1Wb3ItqW0BVSHs=\")"), std::string::npos)
        << login;
    EXPECT_NE(login.find(R"(\"timestamp\":1760448000999,)"), std::string::npos) << login;
    EXPECT_TRUE(login.starts_with(R"({"op":"login","args":["{\"type\":\"login\",\"topics\":[\"ws_zk_accounts_v3\"])"));
}

TEST(ApexOrders, RefusesWhatItCannotSign) {
    RequestBuilder unloaded;
    EXPECT_EQ(unloaded.create_order(k_order_vectors[0].terms, 1).error(), BuildError::NotSigned);
    EXPECT_FALSE(unloaded.load(k_api_key, k_passphrase, k_api_secret, "0x1234"));

    RequestBuilder builder;
    ASSERT_TRUE(builder.load(k_api_key, k_passphrase, k_api_secret, k_seeds));
    auto meta = parse_signing_meta(kept("rest_symbols_signing.json"));
    ASSERT_TRUE(meta.has_value());
    builder.set_meta(std::move(*meta));
    EXPECT_EQ(builder.create_order(k_order_vectors[0].terms, 1).error(), BuildError::NoAccount);
    builder.set_account({.account_id = k_account_id, .taker_fee_rate = e9("0.0005"), .maker_fee_rate = e9("0.0002")});

    auto terms = k_order_vectors[0].terms;
    terms.symbol = "NOPE-USDT";
    EXPECT_EQ(builder.create_order(terms, 1).error(), BuildError::UnknownSymbol);
    terms = k_order_vectors[0].terms;
    terms.client_id = "a&b";
    EXPECT_EQ(builder.create_order(terms, 1).error(), BuildError::BadText);
    terms = k_order_vectors[0].terms;
    terms.size = e9("0");
    EXPECT_EQ(builder.create_order(terms, 1).error(), BuildError::BadAmount);
}

TEST(ApexWire, ReadsSigningMetaAndAccount) {
    const auto meta = parse_signing_meta(kept("rest_symbols_signing.json"));
    ASSERT_TRUE(meta.has_value());
    ASSERT_EQ(meta->contracts.size(), 4U);
    const auto& doge = meta->contracts.at("DOGE-USDT");
    EXPECT_EQ(doge.cross_symbol, "DOGEUSDT");
    EXPECT_EQ(doge.l2_pair_id, 50020U);
    EXPECT_EQ(doge.decimals, 18);

    PrivateBuffer storage;
    const auto account = parse_account(kept("rest_account.json"), storage);
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->terms.account_id, 850000000000000001ULL);
    EXPECT_EQ(account->terms.taker_fee_rate, e9("0.0005"));
    EXPECT_EQ(account->terms.maker_fee_rate, e9("0.0002"));
    EXPECT_TRUE(account->positions.empty());
    EXPECT_FALSE(parse_account(kept("rest_error_timestamp.json"), storage).has_value());
}

TEST(ApexWire, ReadsRepliesAndClassifiesRefusals) {
    const auto created = parse_reply(kept("rest_create_order.json"));
    ASSERT_TRUE(created.has_value());
    EXPECT_TRUE(created->ok);
    EXPECT_EQ(created->order_id, "1234");

    const auto refused = parse_reply(kept("rest_error_timestamp.json"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_FALSE(refused->ok);
    EXPECT_EQ(refused->code, "20002");
    EXPECT_EQ(refused->msg, "err APEX-TIMESTAMP");
    EXPECT_EQ(classify(400, refused->code), ErrorClass::AuthFailure);
    const auto credential = parse_reply(kept("rest_error_credential.json"));
    ASSERT_TRUE(credential.has_value());
    EXPECT_EQ(classify(400, credential->code), ErrorClass::AuthFailure);
    EXPECT_EQ(classify(429, {}), ErrorClass::RateLimited);
    EXPECT_EQ(classify(503, {}), ErrorClass::Retryable);
    EXPECT_EQ(classify(400, "3"), ErrorClass::Terminal);
    EXPECT_FALSE(parse_reply("{}").has_value());
}

TEST(ApexWire, ReadsOrderAndFillPages) {
    PrivateBuffer storage;
    const auto open = parse_rest_rows(kept("rest_open_orders.json"), storage, false);
    ASSERT_TRUE(open.has_value());
    ASSERT_EQ(open->orders.size(), 1U);
    EXPECT_EQ(open->orders[0].client_order_id, "1234");
    EXPECT_EQ(open->orders[0].status, "PENDING");
    EXPECT_EQ(open->orders[0].cum_fill_size, "0.1");
    EXPECT_EQ(open->orders[0].updated_ms, 1647502440973ULL);

    const auto single = parse_rest_rows(kept("rest_create_order.json"), storage, false);
    ASSERT_TRUE(single.has_value());
    ASSERT_EQ(single->orders.size(), 1U);
    EXPECT_EQ(single->orders[0].id, "1234");
    EXPECT_EQ(single->orders[0].client_order_id, "1234");

    const auto fills = parse_rest_rows(kept("rest_fills.json"), storage, true);
    ASSERT_TRUE(fills.has_value());
    ASSERT_EQ(fills->fills.size(), 1U);
    EXPECT_EQ(fills->fills[0].id, "1234");
    EXPECT_EQ(fills->fills[0].order_id, "123546");
    EXPECT_EQ(fills->fills[0].symbol, "BTC-USD");
    EXPECT_EQ(fills->fills[0].fee, "100");
    EXPECT_EQ(fills->fills[0].created_ms, 1647502440973ULL);
}

TEST(ApexWire, ReadsPrivatePushes) {
    const auto ping = parse_private_kind(kept("ws_private_ping.json"));
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->kind, PrivateKind::Ping);
    EXPECT_EQ(ping->value, "1661415022821");

    const auto ack = parse_private_kind(kept("ws_login_ack.json"));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->kind, PrivateKind::Ack);
    EXPECT_TRUE(ack->success);
    EXPECT_EQ(ack->value, "login");

    const auto filled = kept("ws_account_filled.json");
    const auto kind = parse_private_kind(filled);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(kind->kind, PrivateKind::Account);
    EXPECT_FALSE(kind->snapshot);
    PrivateBuffer storage;
    const auto rows = parse_account_push(filled, storage);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->fills.size(), 1U);
    EXPECT_EQ(rows->fills[0].liquidity, "TAKER");
    EXPECT_EQ(rows->fills[0].order_id, "2048046080");
    ASSERT_EQ(rows->orders.size(), 1U);
    EXPECT_EQ(rows->orders[0].status, "FILLED");
    EXPECT_EQ(rows->orders[0].cum_fill_size, "0.500");
    ASSERT_EQ(rows->positions.size(), 1U);
    EXPECT_EQ(rows->positions[0].side, "LONG");

    const auto snapshot = kept("ws_account_snapshot.json");
    const auto snap_kind = parse_private_kind(snapshot);
    ASSERT_TRUE(snap_kind.has_value());
    EXPECT_EQ(snap_kind->kind, PrivateKind::Account);
    EXPECT_TRUE(snap_kind->snapshot);
    const auto snap_rows = parse_account_push(snapshot, storage);
    ASSERT_TRUE(snap_rows.has_value());
    EXPECT_FALSE(snap_rows->orders.empty());
}

TEST(ApexMap, MapsOrdersFillsAndPositions) {
    const auto base = eth_base();
    const auto ts = UnixNanos{1'000};
    PrivateBuffer storage;

    const auto pending = parse_account_push(kept("ws_account_pending.json"), storage);
    ASSERT_TRUE(pending.has_value() && pending->orders.size() == 1U);
    const auto accepted = map_order(pending->orders[0], base, ts);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->type, OrderEventType::Accepted);
    EXPECT_EQ(accepted->leaves_qty.raw, 500);
    EXPECT_EQ(accepted->exchange_client_order_id.view(), "522843990");

    const auto canceled = parse_account_push(kept("ws_account_canceled.json"), storage);
    ASSERT_TRUE(canceled.has_value() && canceled->orders.size() == 1U);
    const auto gone = map_order(canceled->orders[0], base, ts);
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone->type, OrderEventType::Canceled);

    const auto filled = parse_account_push(kept("ws_account_filled.json"), storage);
    ASSERT_TRUE(filled.has_value());
    EXPECT_FALSE(map_order(filled->orders[0], base, ts).has_value()); // the fill row tells
    const auto report = map_order_report(filled->orders[0], base);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Filled);
    const auto fill = map_fill(filled->fills[0], base, QtyLots::from_raw(0), ts);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, OrderEventType::Filled);
    EXPECT_EQ(fill->liquidity, Liquidity::Taker);
    EXPECT_EQ(fill->qty.raw, 500);
    EXPECT_EQ(fill->price.raw, 25'000);
    EXPECT_EQ(fill->fee, e9("0.625"));
    EXPECT_EQ(fill->ts_event.ns, 1652185521361ULL * 1'000'000ULL);
    const auto position = map_position(filled->positions[0], base);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->net_qty.raw, 1'820);
    EXPECT_EQ(position->avg_entry.raw, 25'000);
}

} // namespace
} // namespace raikiri::venues::apex
