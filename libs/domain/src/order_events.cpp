#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/order_events.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace raikiri {

namespace {

// fixed width so two identities cannot collide
constexpr std::size_t k_hex_digits = 16;

void write_hex(std::span<char, k_hex_digits> out, std::uint64_t value) noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = k_hex_digits; i > 0; --i) {
        out[i - 1] = digits[value & 0xFU];
        value >>= 4U;
    }
}

} // namespace

std::optional<AssetCode> to_asset_code(std::string_view text) noexcept {
    if (text.empty() || text.size() > AssetCode{}.data.size()) {
        return std::nullopt;
    }
    std::array<char, 32> folded{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        folded.at(i) = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    const auto code = AssetCode::from(std::string_view{folded.data(), text.size()});
    return code.has_value() ? std::optional{*code} : std::nullopt;
}

ExecId fill_identity(ClientOrderId order, QtyLots leaves, std::string_view venue_exec_id) noexcept {
    if (!venue_exec_id.empty()) {
        if (const auto id = ExecId::from(venue_exec_id); id.has_value()) {
            return *id;
        }
        // too long for the field; truncating would merge two fills
    }
    std::array<char, 1 + k_hex_digits + 1 + k_hex_digits> buf{};
    buf[0] = '~';
    write_hex(std::span<char, k_hex_digits>{buf.data() + 1, k_hex_digits}, order.raw);
    buf[1 + k_hex_digits] = '.';
    write_hex(std::span<char, k_hex_digits>{buf.data() + k_hex_digits + 2, k_hex_digits},
              static_cast<std::uint64_t>(leaves.raw));
    const auto id = ExecId::from(std::string_view{buf.data(), buf.size()});
    return id.has_value() ? *id : ExecId{};
}

} // namespace raikiri
