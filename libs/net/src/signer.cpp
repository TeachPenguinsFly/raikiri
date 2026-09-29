#include <raikiri/net/signer.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstring>

namespace raikiri::net {

namespace {

constexpr char k_hex_digits[] = "0123456789abcdef";
constexpr char k_b64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void to_hex(std::span<const std::uint8_t> digest, char* out) noexcept {
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[2 * i] = k_hex_digits[digest[i] >> 4];
        out[2 * i + 1] = k_hex_digits[digest[i] & 0x0F];
    }
}

// hand-rolled: no allocation, fixed 32-byte input
void to_base64(std::span<const std::uint8_t, k_hmac_bytes> digest, char* out) noexcept {
    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 2 < k_hmac_bytes; i += 3) {
        const std::uint32_t triple = (std::uint32_t{digest[i]} << 16) | (std::uint32_t{digest[i + 1]} << 8) |
                                     digest[i + 2];
        out[o++] = k_b64_digits[(triple >> 18) & 0x3F];
        out[o++] = k_b64_digits[(triple >> 12) & 0x3F];
        out[o++] = k_b64_digits[(triple >> 6) & 0x3F];
        out[o++] = k_b64_digits[triple & 0x3F];
    }
    // 32 % 3 == 2, so exactly one two-byte tail with a single '=' pad.
    const std::uint32_t tail = (std::uint32_t{digest[i]} << 16) | (std::uint32_t{digest[i + 1]} << 8);
    out[o++] = k_b64_digits[(tail >> 18) & 0x3F];
    out[o++] = k_b64_digits[(tail >> 12) & 0x3F];
    out[o++] = k_b64_digits[(tail >> 6) & 0x3F];
    out[o] = '=';
}

} // namespace

bool hmac_sha256(std::string_view key, std::string_view message, std::span<std::uint8_t, k_hmac_bytes> out) noexcept {
    unsigned int len = 0;
    const auto* result = ::HMAC(EVP_sha256(),
                                key.data(),
                                static_cast<int>(key.size()),
                                reinterpret_cast<const unsigned char*>(message.data()),
                                message.size(),
                                out.data(),
                                &len);
    return result != nullptr && len == k_hmac_bytes;
}

std::string_view hmac_sha256_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_hex_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac_bytes> digest{};
    if (!hmac_sha256(key, message, std::span<std::uint8_t, k_hmac_bytes>{digest})) {
        return {};
    }
    to_hex(digest, out.data());
    return {out.data(), k_hex_bytes};
}

std::string_view sha512_hex(std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_hex512_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac512_bytes> digest{};
    unsigned int len = 0;
    // an empty body still hashes; gate signs SHA512("") for every GET
    if (::EVP_Digest(message.data(), message.size(), digest.data(), &len, EVP_sha512(), nullptr) != 1 ||
        len != k_hmac512_bytes) {
        return {};
    }
    to_hex(digest, out.data());
    return {out.data(), k_hex512_bytes};
}

namespace {

std::string_view sha256_pair_hex(std::string_view first, std::string_view second, std::span<char> out) noexcept {
    if (out.size() < k_hex_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac_bytes> digest{};
    unsigned int len = 0;
    auto* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return {};
    }
    const bool ok = ::EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                    ::EVP_DigestUpdate(ctx, first.data(), first.size()) == 1 &&
                    ::EVP_DigestUpdate(ctx, second.data(), second.size()) == 1 &&
                    ::EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1 && len == k_hmac_bytes;
    ::EVP_MD_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    to_hex(digest, out.data());
    return {out.data(), k_hex_bytes};
}

} // namespace

std::string_view sha256_hex(std::string_view message, std::span<char> out) noexcept {
    return sha256_pair_hex(message, {}, out);
}

std::string_view hmac_sha512_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_hex512_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac512_bytes> digest{};
    unsigned int len = 0;
    const auto* result = ::HMAC(EVP_sha512(),
                                key.data(),
                                static_cast<int>(key.size()),
                                reinterpret_cast<const unsigned char*>(message.data()),
                                message.size(),
                                digest.data(),
                                &len);
    if (result == nullptr || len != k_hmac512_bytes) {
        return {};
    }
    to_hex(digest, out.data());
    return {out.data(), k_hex512_bytes};
}

std::string_view hmac_sha256_base64(std::string_view key, std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_base64_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac_bytes> digest{};
    if (!hmac_sha256(key, message, std::span<std::uint8_t, k_hmac_bytes>{digest})) {
        return {};
    }
    to_base64(digest, out.data());
    return {out.data(), k_base64_bytes};
}

void Signer::set_api_key(std::string_view key) noexcept {
    api_key_len_ = std::min(key.size(), k_max_api_key);
    std::memcpy(api_key_.data(), key.data(), api_key_len_);
}

std::string_view Signer::api_key() const noexcept {
    return {api_key_.data(), api_key_len_};
}

std::string_view Signer::sign(std::string_view message, Encoding encoding, std::span<char> out) const noexcept {
    switch (encoding) {
    case Encoding::Hex:
        return hmac_sha256_hex(secret_.view(), message, out);
    case Encoding::Base64:
        return hmac_sha256_base64(secret_.view(), message, out);
    case Encoding::Hex512:
        return hmac_sha512_hex(secret_.view(), message, out);
    case Encoding::Salted:
        return sha256_pair_hex(message, secret_.view(), out);
    }
    return {};
}

} // namespace raikiri::net
