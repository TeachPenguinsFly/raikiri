#pragma once

// The canonical name of a market, so that the same one on two venues is one
// name.
//
// The venues call the perpetual on bitcoin against tether `BTCUSDT`,
// `BTC_USDT`, `XBTUSDTM` and `BTC-USDT-SWAP`. Nothing above the adapters can
// group those without a form they all reduce to, and grouping them is the whole
// job of a cross-venue view: a scanner that cannot tell you those four are one
// market is a scanner that cannot tell you anything.
//
// This lives in the domain and not in the adapters on purpose. Each adapter
// knows where ITS venue keeps the base and the quote — a field on the catalogue
// row, or a separator in the symbol — and that is all it should know. What the
// canonical form IS, and which two spellings name one asset, is a single answer
// the whole system shares, because two adapters answering it separately is how
// half the venues end up in one group and half in another.

#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/instrument.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace raikiri {

// SPOT AND PERPETUALS ONLY. A dated contract's identity includes its expiry and
// an option's includes its strike and side, so `BTC-28AUG26-40000-C` and the
// nineteen hundred rows beside it all reduce to `BTC/USD` — one name for an
// entire option chain, which is worse than no name at all. Those markets keep
// the venue's own symbol, because on them the venue's symbol IS the identity.

// "BTC/USDT", from the two halves the venue named.
//
// Nullopt when either half is missing: a market with no quote currency is a
// price with no unit, and a canonical name that guessed one would group two
// markets that are not the same.
[[nodiscard]] std::optional<SymbolText> canonical_symbol(std::string_view base,
                                                         std::string_view quote) noexcept;

// The two halves out of a symbol that carries them itself — `BTC_USDT`,
// `BTC-USDT`, `btc_usdt`. For the venues that state them in fields, read the
// fields instead; this is for the ones that do not.
//
// Refuses anything with more or fewer than two parts, so `BTC-USDT-SWAP` is a
// refusal rather than a quote currency of "USDT-SWAP". A venue whose symbol has
// a trailing kind is the venue's own business to strip before calling.
[[nodiscard]] std::optional<std::pair<AssetCode, AssetCode>> split_symbol(std::string_view symbol,
                                                                          char separator) noexcept;

} // namespace raikiri
