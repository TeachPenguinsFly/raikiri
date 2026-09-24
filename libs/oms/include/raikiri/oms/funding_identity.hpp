#pragma once

// dedup key for one settlement: packed instrument id, settlement second and strategy
// the socket and the rest ledger mint different ids for the same charge

#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace raikiri::oms {

// Longest possible: 20 digits of packed id, ':', 11 digits of second, ':',
// 5 digits of strategy id.
inline constexpr std::size_t k_max_funding_identity = 38;

// empty only when `into` is smaller than k_max_funding_identity
[[nodiscard]] std::string_view funding_identity(InstrumentId instrument,
                                                UnixNanos settled_at,
                                                std::uint16_t strategy_id,
                                                std::span<char> into) noexcept;

} // namespace raikiri::oms
