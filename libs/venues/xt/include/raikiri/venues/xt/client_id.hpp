#pragma once

// XT's clientOrderId is too short for a ULID, so this re-encodes one.
//
// VERIFIED against XT's own reference (docs/spot/Order/orderPost.mdx): the spot
// field is `^[a-zA-Z0-9_]{4,22}$`. A 26-character ULID does not fit, and the
// two obvious answers are both wrong — truncating it produces an id that no
// longer identifies the order it names, and omitting it leaves every venue
// echo unattributable, so a restart cannot tell our orders from a web order.
//
// A ULID is 26 Crockford base32 characters carrying exactly 128 bits (a 48-bit
// millisecond plus 80 random bits). 62^22 > 2^128 > 62^21, so 22 base62
// characters is the SHORTEST lossless form and XT's ceiling is exactly that.
// The mapping is a bijection: nothing is dropped and nothing is invented, and
// an echo decodes back to the id the OMS minted.
//
// The alphabet is ASCII-ordered, so ids stay lexically sortable by mint time —
// the property the ULID was chosen for in the first place.

#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace raikiri::venues::xt {

inline constexpr std::size_t k_client_id_len = 22;

using XtClientId = FixedString<k_client_id_len>;

// nullopt when the id is not a well-formed 26-character ULID: an id this
// adapter cannot render exactly is refused rather than sent in a shape the
// venue would echo back as something else.
[[nodiscard]] std::optional<XtClientId> encode_client_id(const ExchangeClientOrderId& id) noexcept;

// nullopt for anything that is not one of our own encodings — XT's web orders
// carry ids like "myOrder123", and a foreign id must leave the field UNSET
// rather than decode into a lookalike of an id we minted.
[[nodiscard]] std::optional<ExchangeClientOrderId> decode_client_id(std::string_view text) noexcept;

} // namespace raikiri::venues::xt
