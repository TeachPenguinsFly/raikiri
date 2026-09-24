#include <raikiri/common/int128.hpp>
#include <raikiri/venues/xt/client_id.hpp>

#include <array>
#include <limits>

namespace raikiri::venues::xt {

namespace {

// crockford base32, the ulid alphabet: no I, L, O or U
constexpr std::string_view k_ulid_alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::string_view k_base62_alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

constexpr std::size_t k_ulid_len = 26;
constexpr uint128_t k_base = 62;

// a ulid's leading character never exceeds 7 (128 bits in 130 slots)
constexpr std::uint8_t k_max_leading_digit = 7;

[[nodiscard]] std::optional<std::uint8_t> index_of(std::string_view alphabet, char c) noexcept {
    const auto at = alphabet.find(c);
    return at == std::string_view::npos ? std::nullopt
                                        : std::optional{static_cast<std::uint8_t>(at)};
}

} // namespace

std::optional<XtClientId> encode_client_id(const ExchangeClientOrderId& id) noexcept {
    const auto text = id.view();
    if (text.size() != k_ulid_len) {
        return std::nullopt;
    }
    uint128_t value = 0;
    for (std::size_t i = 0; i < k_ulid_len; ++i) {
        const auto digit = index_of(k_ulid_alphabet, text[i]);
        if (!digit.has_value() || (i == 0 && *digit > k_max_leading_digit)) {
            return std::nullopt;
        }
        value = (value << 5) | *digit;
    }

    XtClientId out{};
    out.len = static_cast<std::uint8_t>(k_client_id_len);
    // fixed-width and zero-padded so ulid ordering survives
    for (std::size_t i = k_client_id_len; i > 0; --i) {
        out.data[i - 1] = k_base62_alphabet[static_cast<std::size_t>(value % k_base)];
        value /= k_base;
    }
    return out;
}

std::optional<ExchangeClientOrderId> decode_client_id(std::string_view text) noexcept {
    if (text.size() != k_client_id_len) {
        return std::nullopt;
    }
    constexpr uint128_t k_max = std::numeric_limits<uint128_t>::max();
    uint128_t value = 0;
    for (const char c : text) {
        const auto digit = index_of(k_base62_alphabet, c);
        if (!digit.has_value()) {
            return std::nullopt;
        }
        // 62^22 exceeds 2^128, so reject values no ulid holds
        if (value > (k_max - *digit) / k_base) {
            return std::nullopt;
        }
        value = value * k_base + *digit;
    }

    ExchangeClientOrderId out{};
    out.len = static_cast<std::uint8_t>(k_ulid_len);
    for (std::size_t i = 0; i < k_ulid_len; ++i) {
        const auto shift = static_cast<unsigned>(125 - 5 * i);
        out.data[i] = k_ulid_alphabet[static_cast<std::size_t>((value >> shift) & 0x1FU)];
    }
    return out;
}

} // namespace raikiri::venues::xt
