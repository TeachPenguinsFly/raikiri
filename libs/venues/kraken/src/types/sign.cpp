#include <raikiri/venues/kraken/sign.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>

namespace raikiri::venues::kraken {

namespace {

constexpr std::size_t k_sha256_bytes = 32;
constexpr std::size_t k_sha512_bytes = 64;

// config::Secret caps a credential at 512 bytes; the decoded form is never
// longer than the printable one, so this cannot be overflowed by a re-keyed
// account. A live Kraken secret decodes to 64.
constexpr std::size_t k_max_secret_bytes = 512;

constexpr std::string_view k_b64_digits =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int b64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// Strict standard base64: padded, no whitespace, no URL alphabet. A secret
// that needs a lenient decoder is a secret that was mangled in transit, and
// guessing at it produces a key that authenticates as nothing.
[[nodiscard]] bool
b64_decode(std::string_view text, std::span<std::uint8_t> out, std::size_t& written) noexcept {
    written = 0;
    if (text.empty() || text.size() % 4 != 0) {
        return false;
    }
    std::size_t pad = 0;
    while (pad < 2 && text[text.size() - 1 - pad] == '=') {
        ++pad;
    }
    // Padding only ever occupies the final one or two slots; an '=' anywhere
    // else is a mangled secret, not a shorter one.
    for (std::size_t i = 0; i + pad < text.size(); ++i) {
        if (text[i] == '=') {
            return false;
        }
    }
    const std::size_t bytes = text.size() / 4 * 3 - pad;
    if (bytes > out.size()) {
        return false;
    }
    for (std::size_t i = 0; i + 4 <= text.size(); i += 4) {
        std::uint32_t quad = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                quad <<= 6;
                continue;
            }
            const int value = b64_value(c);
            if (value < 0) {
                return false;
            }
            quad = (quad << 6) | static_cast<std::uint32_t>(value);
        }
        for (std::size_t j = 0; j < 3 && written < bytes; ++j) {
            out[written++] = static_cast<std::uint8_t>((quad >> (16 - 8 * j)) & 0xFFU);
        }
    }
    return written == bytes;
}

// Writes ceil(n/3)*4 characters. `out` is checked by the caller.
void b64_encode(std::span<const std::uint8_t> bytes, std::span<char> out) noexcept {
    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple =
            (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out[o++] = k_b64_digits[(triple >> 18) & 0x3FU];
        out[o++] = k_b64_digits[(triple >> 12) & 0x3FU];
        out[o++] = k_b64_digits[(triple >> 6) & 0x3FU];
        out[o++] = k_b64_digits[triple & 0x3FU];
    }
    const std::size_t tail = bytes.size() - i;
    if (tail == 0) {
        return;
    }
    std::uint32_t rest = std::uint32_t{bytes[i]} << 16;
    if (tail == 2) {
        rest |= std::uint32_t{bytes[i + 1]} << 8;
    }
    out[o++] = k_b64_digits[(rest >> 18) & 0x3FU];
    out[o++] = k_b64_digits[(rest >> 12) & 0x3FU];
    out[o++] = tail == 2 ? k_b64_digits[(rest >> 6) & 0x3FU] : '=';
    out[o] = '=';
}

} // namespace

std::string_view
authent(std::string_view secret_base64, std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_authent_bytes || secret_base64.empty()) {
        return {};
    }
    std::array<std::uint8_t, k_max_secret_bytes> key{};
    std::size_t key_len = 0;
    if (!b64_decode(secret_base64, key, key_len) || key_len == 0) {
        return {};
    }

    std::array<std::uint8_t, k_sha256_bytes> inner{};
    unsigned int inner_len = 0;
    // An empty message still hashes: a GET with no parameters signs
    // SHA256("" + nonce + path), so a null data pointer must not short-circuit.
    const bool hashed =
        ::EVP_Digest(
            message.data(), message.size(), inner.data(), &inner_len, EVP_sha256(), nullptr) == 1 &&
        inner_len == k_sha256_bytes;

    std::array<std::uint8_t, k_sha512_bytes> outer{};
    unsigned int outer_len = 0;
    const bool signed_ok = hashed &&
                           ::HMAC(EVP_sha512(),
                                  key.data(),
                                  static_cast<int>(key_len),
                                  inner.data(),
                                  inner.size(),
                                  outer.data(),
                                  &outer_len) != nullptr &&
                           outer_len == k_sha512_bytes;
    ::OPENSSL_cleanse(key.data(), key.size());
    if (!signed_ok) {
        return {};
    }

    b64_encode(outer, out);
    return {out.data(), k_authent_bytes};
}

} // namespace raikiri::venues::kraken
