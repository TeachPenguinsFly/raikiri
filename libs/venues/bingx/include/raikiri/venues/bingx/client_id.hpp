#pragma once

// BingX carries a ULID whole — and folds it to LOWERCASE.
//
// The field is `^[a-zA-Z0-9]{1,40}$` on the perpetual tree (VERIFIED against
// BingX's own parameter table) and letters/digits/underscore on spot, so the
// OMS's 26-character ULID fits with room to spare. Three sibling adapters had
// to re-encode theirs — xt into 22 base62 characters, coinbase into a UUIDv4,
// bitfinex into a 48-bit numeric cid — and this one does not: nothing is
// re-encoded, nothing is truncated, and 26 characters travel end to end.
//
// What BingX does do is state, twice in its own documentation, that the field
// is "converted to lowercase by the system". A ULID is Crockford base32 and
// therefore uppercase-only, so lowercasing is injective and uppercasing the
// echo recovers the exact id the OMS minted. Comparing the two without that
// fold makes every one of our own orders look like a stranger's — which is
// how a restart adopts nothing and cancels nothing.

#include <raikiri/domain/identifiers.hpp>

#include <optional>
#include <string_view>

namespace raikiri::venues::bingx {

// The 26 characters a ULID may hold, either case. Anything else is not an id
// this adapter minted and must not be decoded into a lookalike of one.
[[nodiscard]] bool is_client_id(std::string_view text) noexcept;

// The bytes to send. Empty when the id is not a well-formed ULID: an id this
// adapter cannot render exactly is refused rather than sent in a shape the
// venue would echo back as something else.
[[nodiscard]] std::string_view client_id_text(const ExchangeClientOrderId& id) noexcept;

// The echo, folded back to upper case. nullopt for a web order's id, which
// must leave the field UNSET rather than decode into something we own.
[[nodiscard]] std::optional<ExchangeClientOrderId>
client_id_from_text(std::string_view text) noexcept;

} // namespace raikiri::venues::bingx
