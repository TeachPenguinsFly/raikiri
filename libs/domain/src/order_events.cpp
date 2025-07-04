#include <raikiri/domain/order_events.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace raikiri {

namespace {

// Fixed width, so two identities are never ambiguous about where one field
// ends: a variable-length encoding would let 0x1a.0xb and 0x1.0xab collide.
constexpr std::size_t k_hex_digits = 16;

void write_hex(std::span<char, k_hex_digits> out, std::uint64_t value) noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = k_hex_digits; i > 0; --i) {
        out[i - 1] = digits[value & 0xFU];
        value >>= 4U;
    }
}

} // namespace

ExecId fill_identity(ClientOrderId order, QtyLots leaves, std::string_view venue_exec_id) noexcept {
    if (!venue_exec_id.empty()) {
        if (const auto id = ExecId::from(venue_exec_id); id.has_value()) {
            return *id;
        }
        // Longer than the field can hold. Truncating would merge two fills into
        // one row, so this falls through and says the venue did not answer —
        // which is less wrong, and visible.
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
