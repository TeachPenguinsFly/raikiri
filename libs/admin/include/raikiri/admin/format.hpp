#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/instrument.hpp>

#include <string>

namespace raikiri::admin {

// digits, not a json number; ids and timestamps exceed 2^53
[[nodiscard]] std::string digits(std::uint64_t value);

[[nodiscard]] inline std::string digits(InstrumentId id) {
    return digits(id.packed);
}

[[nodiscard]] inline std::string digits(UnixNanos ts) {
    return digits(ts.ns);
}

// empty for an undefined value
[[nodiscard]] std::string decimal(FixedE9 value);
[[nodiscard]] std::string decimal(FixedE6 value);

[[nodiscard]] std::string price(PriceTicks ticks, const InstrumentBase& base);
[[nodiscard]] std::string size(QtyLots lots, const InstrumentBase& base);

[[nodiscard]] std::string_view contract_kind_name(ContractKind kind) noexcept;

} // namespace raikiri::admin
