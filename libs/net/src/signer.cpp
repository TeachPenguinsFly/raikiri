#include <raikiri/net/signer.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstring>

namespace raikiri::net {

namespace {

constexpr char k_hex_digits[] = "0123456789abcdef";
constexpr char k_b64_digits[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void to_hex(std::span<const std::uint8_t, k_hmac_bytes> digest, char* out) noexcept {
    for (std::size_t i = 0; i < k_hmac_bytes; ++i) {
        out[2 * i] = k_hex_digits[digest[i] >> 4];
        out[2 * i + 1] = k_hex_digits[digest[i] & 0x0F];
    }
}

// Hand-rolled rather than OpenSSL's EVP_EncodeBlock: no allocation, no
// newline behavior to remember, and the input length is always 32.
void to_base64(std::span<const std::uint8_t, k_hmac_bytes> digest, char* out) noexcept {
    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 2 < k_hmac_bytes; i += 3) {
        const std::uint32_t triple =
            (std::uint32_t{digest[i]} << 16) | (std::uint32_t{digest[i + 1]} << 8) | digest[i + 2];
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

bool hmac_sha256(std::string_view key, std::string_view message,
                 std::span<std::uint8_t, k_hmac_bytes> out) noexcept {
    unsigned int len = 0;
    const auto* result =
        ::HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
               reinterpret_cast<const unsigned char*>(message.data()), message.size(), out.data(),
               &len);
    return result != nullptr && len == k_hmac_bytes;
}

std::string_view hmac_sha256_hex(std::string_view key, std::string_view message,
                                 std::span<char> out) noexcept {
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

std::string_view hmac_sha256_base64(std::string_view key, std::string_view message,
                                    std::span<char> out) noexcept {
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

std::string_view Signer::sign(std::string_view message, Encoding encoding,
                              std::span<char> out) const noexcept {
    return encoding == Encoding::Hex ? hmac_sha256_hex(secret_.view(), message, out)
                                     : hmac_sha256_base64(secret_.view(), message, out);
}

} // namespace raikiri::net
