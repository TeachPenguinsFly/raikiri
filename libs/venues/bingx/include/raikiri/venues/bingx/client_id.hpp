#pragma once

// bingx lowercases client ids; a ulid is uppercase-only, so uppercasing the echo recovers it

#include <raikiri/domain/identifiers.hpp>

#include <optional>
#include <string_view>

namespace raikiri::venues::bingx {

[[nodiscard]] bool is_client_id(std::string_view text) noexcept;

// empty when the id is not a well-formed ulid
[[nodiscard]] std::string_view client_id_text(const ExchangeClientOrderId& id) noexcept;

// the echo uppercased; nullopt for an id this adapter did not mint
[[nodiscard]] std::optional<ExchangeClientOrderId>
client_id_from_text(std::string_view text) noexcept;

} // namespace raikiri::venues::bingx
