#pragma once

// The account itself, as opposed to what it holds. HTX publishes no balance
// or ledger feed this adapter reads; what it does publish is an account
// identity the spot tree refuses to trade without.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::htx {

// The `id` of the first working account of type "spot" from
// GET /v1/account/accounts. Every spot order carries it, and there is no
// default: an order without account-id is refused as an invalid parameter.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_spot_account_id(std::string_view body);

} // namespace raikiri::venues::htx
