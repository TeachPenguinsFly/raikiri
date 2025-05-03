#pragma once

// Wire helpers specific to KuCoin; the shared readers live in raikiri::wire.
//
// What differs is below — KuCoin puts its timestamps and half its decimals in
// JSON numbers, so the scalar readers work from the raw token rather than from
// a string.

#include <raikiri/venues/kucoin/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin::detail {
using namespace raikiri::wire;

[[nodiscard]] std::expected<std::string_view, WireError> raw_of(ondemand::value value);

// Numeric text with surrounding quotes removed, so that tickerV2's quoted prices
// and its bare sizes reach the DTO through one reader.
[[nodiscard]] std::expected<std::string_view, WireError> opt_decimal(ondemand::object& obj,
                                                                     std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_decimal(ondemand::object& obj,
                                                                     std::string_view key);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

// Both transports wrap the payload in `data`, and unlike okx it is a single
// object on every channel and on every singleton REST read.
template <typename Fn>
[[nodiscard]] auto with_data(ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<ondemand::object&>())) {
    auto obj = req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}
} // namespace raikiri::venues::kucoin::detail
