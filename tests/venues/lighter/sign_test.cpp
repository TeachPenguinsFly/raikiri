#include <raikiri/venues/lighter/sign.hpp>
#include <raikiri/venues/lighter/tx.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// vectors printed by the reference go signer (poseidon_crypto v0.0.15), its random nonce
// swapped for the same sha-512 derivation this signer uses

namespace raikiri::venues::lighter {
namespace {

constexpr std::uint64_t k_p = 0xFFFFFFFF00000001ULL;

constexpr std::string_view k_key = "9a1c7e3e2f4b5d6a7c8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f6071829304a5b6";

std::array<std::uint8_t, 40> bytes40(std::string_view hex) {
    std::array<std::uint8_t, 40> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
    }
    return out;
}

std::array<std::uint8_t, 40> small_scalar(std::uint64_t value) {
    std::array<std::uint8_t, 40> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    return out;
}

TxHeader header(std::int64_t nonce, bool skip = false) {
    return {.account_index = 281474976710654,
            .api_key_index = 3,
            .expired_at = 1700000600000,
            .nonce = nonce,
            .skip_nonce = skip};
}

TEST(LighterSign, Poseidon2MatchesReference) {
    const std::vector<std::pair<std::vector<std::uint64_t>, std::string_view>> cases{
        {{}, "00000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {{1, 2, 3, 4, 5}, "4e44c5b57ad54f8038d6c278274b9fedd306f094f09f206ba3b27295896af86c3b1630503526febf"},
        {{0, 1, 2, 3, 4, 5, 6, 7}, "ff2e86b62bf0565d2542f57c854ad38b7b722227e65b6f7b140ffbea25ae36d0e51521f8446dc30b"},
        {{0, 1, 2, 3, 4, 5, 6, 7, 8},
         "130676cf339cd3c59d37b533125292cf4a1993a87b43b2edbfb0a4c057a09e5c42abd14b9ccf0133"},
        {{k_p - 1, ~std::uint64_t{0}, 0, std::uint64_t{1} << 63, k_p, 12345678901234567},
         "48e3f30fb603be89778aac38a85f669e79e1e3f656f636c6926a23dd014eb0011ca3c9a9c85ace0a"},
    };
    for (const auto& [input, expected] : cases) {
        EXPECT_EQ(to_hex(hash_to_quintic(input)), expected);
    }
}

TEST(LighterSign, PublicKeysMatchReference) {
    ApiKey one;
    ASSERT_TRUE(one.load("01" + std::string(78, '0')));
    EXPECT_EQ(to_hex(one.public_key()),
              "04000000000000000000000000000000000000000000000000000000000000000000000000000000");

    ApiKey key;
    ASSERT_TRUE(key.load(std::string{"0x"} + std::string{k_key}));
    EXPECT_EQ(to_hex(key.public_key()),
              "6fc4eb42c0340d35222df4e16a750a77a8ff58320a88b8ee64798934d54eeaf7b08d05a4d63068bf");

    // above the group order, reduced as the venue reduces it
    ApiKey wide;
    ASSERT_TRUE(wide.load(std::string(80, 'f')));
    EXPECT_EQ(to_hex(wide.public_key()),
              "62a8adf6a8ae5960fa627364fb72c85135ccc540b5e191ef78dddc04e9b038d6efd625c01458d090");
    EXPECT_EQ(to_hex(encode_base_mul(
                  bytes40("3d00e8d6d24ce02fc6beb6518c47ef2e8cf38f6032000000d3ffffff1d000000f1ffffff04000000"))),
              "62a8adf6a8ae5960fa627364fb72c85135ccc540b5e191ef78dddc04e9b038d6efd625c01458d090");
}

TEST(LighterSign, RefusesMalformedKeys) {
    ApiKey key;
    EXPECT_FALSE(key.load(""));
    EXPECT_FALSE(key.load(k_key.substr(2)));
    EXPECT_FALSE(key.load(std::string{"zz"} + std::string{k_key.substr(2)}));
    EXPECT_FALSE(key.load(std::string(80, '0')));
    EXPECT_FALSE(key.loaded());
}

TEST(LighterSign, SchnorrWithAGivenNonceMatchesReference) {
    const auto secret = bytes40(k_key);
    std::array<std::uint64_t, 3> preimage{1, 2, 3};
    const auto digest = hash_to_quintic(preimage);
    EXPECT_EQ(to_hex(digest), "eba7ee38133f127ddaa119070a5c51d0c65bb35ab5c4b037df453847037f9081b13ba509cf216e2d");
    const std::vector<std::pair<std::uint64_t, std::string_view>> cases{
        {1,
         "250ec11884d2a6dce8d7970cb1c58aa2af725aaabe67c4553f4683e9a03da7a59934d530053bea21da18378cb72dc40bfe3d6464f444c"
         "beb"
         "02e2426f08bc6abffe2308dc790bc5bca4ad4e6125131e45"},
        {2,
         "c229be26270a5629c90df58d87836b08cde4cb024d716c11779d9c6710101332473e5a78c97fad433fe72d5242bdd5c1ff67e22e129c6"
         "5a0"
         "854a65da47c986051c8fca1b9c6ec5875552d8ef72e41672"},
        {0xdeadbeef,
         "fb8d025038a1fdeddadb22948d9a6d892e47ae7655d1f6bcdde32466b1c1b935b812a39fc3e97c1e2d0c5d405c2db4d2bfcc8dc0ab532"
         "c20"
         "899e7aca6507c0b4f7b1c1b5c73007298a0439c9a7e21566"},
    };
    for (const auto& [nonce, expected] : cases) {
        EXPECT_EQ(to_hex(sign_with_nonce(secret, digest, small_scalar(nonce))), expected);
    }
}

TEST(LighterSign, DerivedNonceSignsDeterministically) {
    ApiKey key;
    ASSERT_TRUE(key.load(k_key));
    std::array<std::uint64_t, 3> preimage{1, 2, 3};
    const auto digest = hash_to_quintic(preimage);
    const auto first = key.sign(digest);
    EXPECT_EQ(
        to_hex(first),
        "627b4b4a1df46809ceaed8f53cb6990901f5e89b0bf322aac359c06170f7fef7be514d97f307c11b281e72ee269e69c7687529d41090"
        "adb6ee14a31b77fd362f9c5ac583a11e93b3a3fe6b590a8fb93c");
    EXPECT_EQ(key.sign(digest), first);
}

TEST(LighterTx, CreateOrderMatchesReference) {
    ApiKey key;
    ASSERT_TRUE(key.load(k_key));
    CreateOrderTx tx{.header = header(7),
                     .market_index = 0,
                     .client_order_index = 1,
                     .base_amount = 1000,
                     .price = 4294967295U,
                     .is_ask = 1,
                     .type = k_order_limit,
                     .time_in_force = k_tif_ioc};
    const auto digest = tx_hash(tx, k_chain_live);
    EXPECT_EQ(to_hex(digest), "91a332d396720cddc8596d67489e46ab2ad847df14e6d3a5cd557d5699ea026329624a9a218bf82a");
    EXPECT_EQ(
        tx_info(tx, key.sign(digest)),
        R"({"AccountIndex":281474976710654,"ApiKeyIndex":3,"MarketIndex":0,"ClientOrderIndex":1,"BaseAmount":1000,)"
        R"("Price":4294967295,"IsAsk":1,"Type":0,"TimeInForce":0,"ReduceOnly":0,"TriggerPrice":0,"OrderExpiry":0,)"
        R"("ExpiredAt":1700000600000,"Nonce":7,)"
        R"("Sig":"dSJEWUQcprxPIB44Xunz79gcoFtLD/Q+oyoogXqTBKiP6GsHU06ZGeiLFZIST8/61hf344soPYfUlsYh5Mi9/wi0HRPaxzRJhuj44MzFj2Y=",)"
        R"("L2TxAttributes":null})");

    tx.header.skip_nonce = true;
    const auto skipped = tx_hash(tx, k_chain_live);
    EXPECT_EQ(to_hex(skipped), "38b685084ce5fc2b0d9589dfb3693195721b1eff1a3b9d3a6c454bc432f61b1f6e48d8ee9e2da2f9");
    EXPECT_TRUE(
        tx_info(tx, key.sign(skipped))
            .ends_with(
                R"("Sig":"mQq3CJbDOmOEQfdT1o3qwWENthZ3BuPPK5nQnjeWfZzL3/pnjd6JZbQ9mGeg/PT43vXRIlcA8z0gVGAnrEZAeo5/pHXc61fNhN6WASqzgGE=",)"
                R"("L2TxAttributes":{"4":1}})"));
}

TEST(LighterTx, PostOnlyOnTestnetMatchesReference) {
    ApiKey key;
    ASSERT_TRUE(key.load(k_key));
    const CreateOrderTx tx{.header = header(8),
                           .market_index = 24,
                           .client_order_index = k_max_client_order_index,
                           .base_amount = 52,
                           .price = 312345,
                           .is_ask = 0,
                           .type = k_order_limit,
                           .time_in_force = k_tif_post_only,
                           .order_expiry = 1702419200000};
    const auto digest = tx_hash(tx, k_chain_test);
    EXPECT_EQ(to_hex(digest), "6cda7d38569b5774b14719c980e0de45e63da9e13dbcd44d3e5e7897388ebf0035d3b9c1c36e1e72");
    EXPECT_EQ(
        tx_info(tx, key.sign(digest)),
        R"({"AccountIndex":281474976710654,"ApiKeyIndex":3,"MarketIndex":24,"ClientOrderIndex":281474976710655,)"
        R"("BaseAmount":52,"Price":312345,"IsAsk":0,"Type":0,"TimeInForce":2,"ReduceOnly":0,"TriggerPrice":0,)"
        R"("OrderExpiry":1702419200000,"ExpiredAt":1700000600000,"Nonce":8,)"
        R"("Sig":"qpPM8PCGg1I3kuMo5dt3Fr8xBbtfYdljmsC22H3KxcQhOF1LYqmhbd7eikA82IhTL6mX5pmuOKWSFfE3GTWQmskBVek2hDEU4ZSMb0Is8FU=",)"
        R"("L2TxAttributes":null})");
}

TEST(LighterTx, CancelMatchesReference) {
    ApiKey key;
    ASSERT_TRUE(key.load(k_key));
    const CancelOrderTx tx{.header = header(9), .market_index = 1, .index = 42};
    const auto digest = tx_hash(tx, k_chain_live);
    EXPECT_EQ(to_hex(digest), "5353631a4fdcbd7570252f873a0a311cf92fe81d8331dc15ae8bfab9a1904ba9e644f368c8f8e38f");
    EXPECT_EQ(
        tx_info(tx, key.sign(digest)),
        R"({"AccountIndex":281474976710654,"ApiKeyIndex":3,"MarketIndex":1,"Index":42,"ExpiredAt":1700000600000,)"
        R"("Nonce":9,"Sig":"F2+yRaU58s4dJY1H7xsJmDuiGgU2a2xadRYioRE2gH/x89D+4BdzQjJk9eg+QAtqiaR8KhmJw5QtfNEJM7D8LNfYaui9MzsbyvVVIafoskU=",)"
        R"("L2TxAttributes":null})");
}

TEST(LighterTx, AuthTokenMatchesReference) {
    ApiKey key;
    ASSERT_TRUE(key.load(k_key));
    EXPECT_EQ(
        auth_token(key, 1700000600, 281474976710654, 3),
        "1700000600:281474976710654:3:d82d2e89c2d3ad9bad9dd78f3cca1b3b030d3e63cefdddf04e0105ed8517fbfe96254a43f8aaca"
        "40a0a9376be9acf1c606915891f8e9d44940564b5f9adcd420a6eb5239a366e7a0adafd6cceb589d24");
}

} // namespace
} // namespace raikiri::venues::lighter
