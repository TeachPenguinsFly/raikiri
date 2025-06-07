#include <raikiri/venues/bitfinex/sign.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <charconv>
#include <cstring>

namespace raikiri::venues::bitfinex {

namespace {

constexpr char k_hex_digits[] = "0123456789abcdef";

} // namespace

std::string_view
hmac_sha384_hex(std::string_view key, std::string_view message, std::span<char> out) noexcept {
    if (out.size() < k_hex384_bytes) {
        return {};
    }
    std::array<std::uint8_t, k_hmac384_bytes> digest{};
    unsigned int len = 0;
    const auto* result = ::HMAC(EVP_sha384(),
                                key.data(),
                                static_cast<int>(key.size()),
                                reinterpret_cast<const unsigned char*>(message.data()),
                                message.size(),
                                digest.data(),
                                &len);
    if (result == nullptr || len != k_hmac384_bytes) {
        return {};
    }
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out[2 * i] = k_hex_digits[digest[i] >> 4];
        out[2 * i + 1] = k_hex_digits[digest[i] & 0x0F];
    }
    return {out.data(), k_hex384_bytes};
}

std::string_view ws_auth_frame(std::string_view api_key,
                               std::string_view secret,
                               std::uint64_t nonce,
                               std::span<char> out) noexcept {
    if (api_key.empty() || secret.empty()) {
        return {};
    }
    std::array<char, 24> digits{};
    const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), nonce);
    if (ec != std::errc{}) {
        return {};
    }
    const std::string_view nonce_text{digits.data(),
                                      static_cast<std::size_t>(stop - digits.data())};

    // THE NONCE APPEARS TWICE. Once is answered 10100, which reads as a
    // revoked key rather than as a malformed payload.
    std::array<char, 64> payload{};
    if (4 + nonce_text.size() * 2 > payload.size()) {
        return {};
    }
    std::memcpy(payload.data(), "AUTH", 4);
    std::memcpy(payload.data() + 4, nonce_text.data(), nonce_text.size());
    std::memcpy(payload.data() + 4 + nonce_text.size(), nonce_text.data(), nonce_text.size());
    const std::string_view payload_text{payload.data(), 4 + nonce_text.size() * 2};

    std::array<char, k_hex384_bytes> signature{};
    const auto sig = hmac_sha384_hex(secret, payload_text, signature);
    if (sig.empty()) {
        return {};
    }

    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > out.size()) {
            overflow = true;
            return;
        }
        std::memcpy(out.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(R"({"event":"auth","apiKey":")");
    put(api_key);
    put(R"(","authSig":")");
    put(sig);
    put(R"(","authPayload":")");
    put(payload_text);
    put(R"(","authNonce":)");
    put(nonce_text);
    put("}");
    if (overflow) {
        return {};
    }
    return {out.data(), len};
}

std::uint64_t NonceSource::next(std::uint64_t wall_ns) noexcept {
    const auto micros = wall_ns / 1'000ULL;
    last_ = micros > last_ ? micros : last_ + 1;
    return last_;
}

} // namespace raikiri::venues::bitfinex
