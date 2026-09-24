#pragma once

// xt's clientOrderId is ^[a-zA-Z0-9_]{4,22}$, so the 128-bit ulid is re-encoded as 22 base62 chars;
// the ascii-ordered alphabet keeps ids sortable by mint time

#include <raikiri/domain/identifiers.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace raikiri::venues::xt {

inline constexpr std::size_t k_client_id_len = 22;

using XtClientId = FixedString<k_client_id_len>;

// nullopt unless the id is a well-formed 26-character ulid
[[nodiscard]] std::optional<XtClientId> encode_client_id(const ExchangeClientOrderId& id) noexcept;

// nullopt for foreign ids such as web orders' "myOrder123"
[[nodiscard]] std::optional<ExchangeClientOrderId> decode_client_id(std::string_view text) noexcept;

} // namespace raikiri::venues::xt
