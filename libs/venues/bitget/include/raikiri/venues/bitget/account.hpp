#pragma once

#include <raikiri/venues/bitget/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitget {

// one /api/v2/mix/account/bill row; amount is already signed
// fee is not read since the fill already booked the commission
struct WireBill {
    std::string_view bill_id;
    std::string_view symbol; // "BTCUSDT", as the catalogue spells it
    std::string_view coin;
    std::string_view business_type;
    std::string_view amount;
    std::uint64_t c_time_ms{0};
};

// the venue caps limit at 100; one poll takes one page
inline constexpr std::size_t k_max_bills = 128;

struct BillBuffer {
    std::array<WireBill, k_max_bills> bills{};
};

// rows sit under data.bills, beside the endId cursor
[[nodiscard]] std::expected<std::span<const WireBill>, WireError> parse_mix_bills(std::string_view body,
                                                                                  BillBuffer& storage);

} // namespace raikiri::venues::bitget
