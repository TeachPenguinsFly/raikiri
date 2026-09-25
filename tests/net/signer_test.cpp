#include <raikiri/net/signer.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace raikiri::net {
namespace {

// rfc 4231 test case 2
constexpr std::string_view k_rfc_key = "Jefe";
constexpr std::string_view k_rfc_msg = "what do ya want for nothing?";
constexpr std::string_view k_rfc_hex = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";

TEST(Hmac, MatchesRfc4231Vector) {
    std::array<char, k_hex_bytes> out{};
    EXPECT_EQ(hmac_sha256_hex(k_rfc_key, k_rfc_msg, out), k_rfc_hex);
}

TEST(Hmac, Base64MatchesTheSameDigest) {
    std::array<char, k_base64_bytes> out{};
    const auto b64 = hmac_sha256_base64(k_rfc_key, k_rfc_msg, out);
    ASSERT_EQ(b64.size(), k_base64_bytes);
    EXPECT_TRUE(b64.ends_with('='));
    EXPECT_EQ(b64, "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");
}

TEST(Hmac, RejectsUndersizedBuffers) {
    std::array<char, k_hex_bytes - 1> small{};
    EXPECT_TRUE(hmac_sha256_hex(k_rfc_key, k_rfc_msg, small).empty());
    std::array<char, k_base64_bytes - 1> small_b64{};
    EXPECT_TRUE(hmac_sha256_base64(k_rfc_key, k_rfc_msg, small_b64).empty());
}

TEST(Hmac, DistinctMessagesAndKeysDiffer) {
    std::array<char, k_hex_bytes> a{};
    std::array<char, k_hex_bytes> b{};
    const std::string sig1{hmac_sha256_hex("k", "message-one", a)};
    const std::string sig2{hmac_sha256_hex("k", "message-two", b)};
    EXPECT_NE(sig1, sig2);
    const std::string sig3{hmac_sha256_hex("other", "message-one", b)};
    EXPECT_NE(sig1, sig3);
}

TEST(Signer, SignsWithTheHeldSecretAndNeverExposesIt) {
    Signer signer;
    signer.set_api_key("PUBLIC-KEY-ID");
    ASSERT_TRUE(signer.secret().assign(k_rfc_key).has_value());
    EXPECT_EQ(signer.api_key(), "PUBLIC-KEY-ID");

    std::array<char, k_hex_bytes> out{};
    EXPECT_EQ(signer.sign(k_rfc_msg, Signer::Encoding::Hex, out), k_rfc_hex);

    std::array<char, k_base64_bytes> b64{};
    EXPECT_EQ(signer.sign(k_rfc_msg, Signer::Encoding::Base64, b64).size(), k_base64_bytes);
}

TEST(Signer, BybitStyleMessageShape) {
    Signer signer;
    signer.set_api_key("abc123");
    ASSERT_TRUE(signer.secret().assign("topsecret").has_value());

    std::array<char, 256> msg_buf{};
    const std::string_view body = R"({"symbol":"BTCUSDT"})";
    int n = std::snprintf(msg_buf.data(),
                          msg_buf.size(),
                          "%s%s%s%.*s",
                          "1700000000000",
                          "abc123",
                          "5000",
                          static_cast<int>(body.size()),
                          body.data());
    ASSERT_GT(n, 0);

    std::array<char, k_hex_bytes> sig{};
    const auto out = signer.sign({msg_buf.data(), static_cast<std::size_t>(n)}, Signer::Encoding::Hex, sig);
    EXPECT_EQ(out.size(), k_hex_bytes);
    EXPECT_EQ(out.find_first_not_of("0123456789abcdef"), std::string_view::npos);
}

} // namespace
} // namespace raikiri::net
