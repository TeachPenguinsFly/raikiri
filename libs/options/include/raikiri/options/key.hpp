#pragma once

#include <raikiri/domain/instrument.hpp>

#include <compare>
#include <optional>
#include <string>

namespace raikiri::options {

// the same contract on any venue; strike is in quote units
struct OptionKey {
    UnderlyingText underlying{};
    UnixNanos expiry{};
    FixedE9 strike{};
    PutCall put_call{PutCall::Call};

    friend auto operator<=>(const OptionKey&, const OptionKey&) = default;
};

// nullopt when the instrument names no underlying, strike or expiry
[[nodiscard]] std::optional<OptionKey> key_of(const OptionInstrument& option) noexcept;

// UND-YYYYMMDD-STRIKE-C
[[nodiscard]] std::string canonical_name(const OptionKey& key);

} // namespace raikiri::options
