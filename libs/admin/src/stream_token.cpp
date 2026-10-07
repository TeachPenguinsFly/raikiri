#include <raikiri/admin/routes.hpp>
#include <raikiri/admin/stream_token.hpp>
#include <raikiri/net/signer.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <span>

namespace raikiri::admin {

namespace {

constexpr std::size_t k_nonce_bytes = 12;
constexpr std::size_t k_secret_bytes = 32;

std::string hex(std::span<const unsigned char> bytes) {
    static constexpr std::string_view k_digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(k_digits[byte >> 4U]);
        out.push_back(k_digits[byte & 0xFU]);
    }
    return out;
}

bool lower_hex(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

std::string_view to_string(TokenCheck check) noexcept {
    switch (check) {
    case TokenCheck::Valid:
        return "valid";
    case TokenCheck::Missing:
        return "missing";
    case TokenCheck::Malformed:
        return "malformed";
    case TokenCheck::Expired:
        return "expired";
    case TokenCheck::BadSignature:
        return "bad signature";
    }
    return "unknown";
}

bool StreamTokens::randomize() noexcept {
    std::array<unsigned char, k_secret_bytes> raw{};
    if (::RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return false;
    }
    const auto ok = secret_.assign(hex(raw)).has_value();
    ::OPENSSL_cleanse(raw.data(), raw.size());
    return ok;
}

IssuedToken StreamTokens::issue(UnixNanos now) const {
    std::array<unsigned char, k_nonce_bytes> nonce{};
    if (secret_.empty() || ::RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return {};
    }
    const UnixNanos expires_at{now.ns + k_stream_token_ttl_ns};
    auto token = std::to_string(expires_at.ns) + '.' + hex(nonce);
    std::array<char, net::k_hex_bytes> mac{};
    const auto signature = net::hmac_sha256_hex(secret_.view(), token, mac);
    if (signature.empty()) {
        return {};
    }
    token += '.';
    token += signature;
    return {.token = std::move(token), .expires_at = expires_at};
}

TokenCheck StreamTokens::check(std::string_view token, UnixNanos now) const noexcept {
    if (token.empty()) {
        return TokenCheck::Missing;
    }
    const auto first = token.find('.');
    const auto last = token.rfind('.');
    if (first == std::string_view::npos || first == last) {
        return TokenCheck::Malformed;
    }
    const auto expiry_text = token.substr(0, first);
    const auto nonce = token.substr(first + 1, last - first - 1);
    const auto signature = token.substr(last + 1);
    std::uint64_t expiry = 0;
    const auto parsed = std::from_chars(expiry_text.data(), expiry_text.data() + expiry_text.size(), expiry);
    if (parsed.ec != std::errc{} || parsed.ptr != expiry_text.data() + expiry_text.size() ||
        nonce.size() != k_nonce_bytes * 2 || !lower_hex(nonce) || signature.size() != net::k_hex_bytes ||
        !lower_hex(signature)) {
        return TokenCheck::Malformed;
    }
    if (secret_.empty()) {
        return TokenCheck::BadSignature;
    }
    std::array<char, net::k_hex_bytes> mac{};
    const auto expected = net::hmac_sha256_hex(secret_.view(), token.substr(0, last), mac);
    if (expected.size() != signature.size() ||
        ::CRYPTO_memcmp(expected.data(), signature.data(), signature.size()) != 0) {
        return TokenCheck::BadSignature;
    }
    return now.ns < expiry ? TokenCheck::Valid : TokenCheck::Expired;
}

std::optional<http::Response> StreamTokenRoute::operator()(const http::Request& req) const {
    if (req.path != k_stream_token_path) {
        return std::nullopt;
    }
    if (req.method != "GET") {
        return problem(405, "method not allowed", "this path serves reads");
    }
    const auto issued = tokens_->issue(wall_());
    if (issued.token.empty()) {
        return problem(500, "no stream token", "the token could not be signed");
    }
    return http::Response{.status = 200,
                          .content_type = "application/json",
                          .body = R"({"token":")" + issued.token + R"(","expires_at":")" +
                                  std::to_string(issued.expires_at.ns) + "\"}"};
}

} // namespace raikiri::admin
