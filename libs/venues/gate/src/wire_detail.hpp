#pragma once

// Wire helpers specific to Gate; the shared readers live in raikiri::wire.
//
// What differs is below — Gate's numeric fields arrive quoted or bare, so
// scalars are read from the raw token with the quotes optional.

#include <raikiri/venues/gate/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::gate::detail {
using namespace raikiri::wire;

// Reads a numeric field whatever its JSON type. Gate quotes prices and leaves
// sizes bare, and its own SDK disagrees with the live endpoint about
// `order_size_min`, so requiring either encoding would break on real data.
[[nodiscard]] std::expected<std::string_view, WireError> num_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_num(ondemand::object& obj,
                                                                 std::string_view key);

// Whole part of a decimal token. Gate types several epoch fields as float64 in
// its own SDK and ships the REST book's `current` as 1786394534.46, so the
// fraction is truncated rather than making the field unreadable. Not
// parse_decimal_e9: that scales by 1e9 and an epoch-ms value overflows int64.
// Anything else yields 0, which the mapper reads as "no venue time".
[[nodiscard]] std::uint64_t whole_u64(std::string_view s) noexcept;

// A numeric field as a whole number; absent yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);

// A level is {"p":"64156.4","s":35680} on the websocket and in the REST
// snapshot alike — same object either way, unlike okx's positional arrays.
[[nodiscard]] std::expected<std::size_t, WireError>
read_levels(ondemand::object& obj, std::string_view key, std::span<WireLevel> into);

// Every websocket payload hangs off `result`. book_ticker and
// order_book_update put an object there; trades and tickers put an array whose
// elements are the objects.
template <typename Fn>
[[nodiscard]] auto with_result_object(ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<ondemand::object&>())) {
    auto obj = req_object(root, "result");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}
} // namespace raikiri::venues::gate::detail
