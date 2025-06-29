#pragma once

// Domain words becoming contract strings, in one place.
//
// This is the edge schema/admin-api.yaml describes: a price stops being ticks
// and becomes an exact decimal; an instrument id stops being a packed word and
// becomes digits. Both because JavaScript has one numeric type that holds
// integers exactly only to 2^53, and a nanosecond timestamp is already 1.79e18.
//
// Every conversion lives here so there is one answer per question. A second
// place that rendered a price would eventually render it differently.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>

#include <string>

namespace raikiri::admin {

// Digits, never a JSON number. Ids carry the venue at bit 56 and timestamps
// are past 2^53, so both would arrive at a browser rounded.
[[nodiscard]] std::string digits(std::uint64_t value);

[[nodiscard]] inline std::string digits(InstrumentId id) { return digits(id.packed); }
[[nodiscard]] inline std::string digits(UnixNanos ts) { return digits(ts.ns); }

// An exact decimal. Empty for an undefined value — the caller decides whether
// that is a field to omit or a bug, because those are different questions.
[[nodiscard]] std::string decimal(FixedE9 value);

// Ticks and lots resolved through the instrument that gives them meaning. A
// tick count without its instrument is not a price.
[[nodiscard]] std::string price(PriceTicks ticks, const InstrumentBase& base);
[[nodiscard]] std::string size(QtyLots lots, const InstrumentBase& base);

[[nodiscard]] std::string_view contract_kind_name(ContractKind kind) noexcept;

} // namespace raikiri::admin
