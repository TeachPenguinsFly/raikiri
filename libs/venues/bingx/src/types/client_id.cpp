#include <raikiri/venues/bingx/client_id.hpp>

#include <cstddef>

namespace raikiri::venues::bingx {

namespace {

// Crockford base32, the ULID alphabet: no I, L, O or U.
constexpr std::string_view k_ulid_alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

constexpr std::size_t k_ulid_len = 26;

[[nodiscard]] char upper(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

} // namespace

bool is_client_id(std::string_view text) noexcept {
    if (text.size() != k_ulid_len) {
        return false;
    }
    for (const char c : text) {
        if (k_ulid_alphabet.find(upper(c)) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

std::string_view client_id_text(const ExchangeClientOrderId& id) noexcept {
    const auto text = id.view();
    return is_client_id(text) ? text : std::string_view{};
}

std::optional<ExchangeClientOrderId> client_id_from_text(std::string_view text) noexcept {
    if (!is_client_id(text)) {
        return std::nullopt;
    }
    ExchangeClientOrderId out{};
    out.len = static_cast<std::uint8_t>(k_ulid_len);
    for (std::size_t i = 0; i < k_ulid_len; ++i) {
        out.data[i] = upper(text[i]);
    }
    return out;
}

} // namespace raikiri::venues::bingx
