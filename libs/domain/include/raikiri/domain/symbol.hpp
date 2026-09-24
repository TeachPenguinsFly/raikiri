#pragma once

// the canonical cross-venue name of a market

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace raikiri {

// spot and perpetuals only; dated contracts and options keep the venue's symbol

// nullopt when either half is missing
[[nodiscard]] std::optional<SymbolText> canonical_symbol(std::string_view base,
                                                         std::string_view quote) noexcept;

// splits `BTC_USDT`-style symbols; refuses anything but exactly two parts
[[nodiscard]] std::optional<std::pair<AssetCode, AssetCode>> split_symbol(std::string_view symbol,
                                                                          char separator) noexcept;

} // namespace raikiri
