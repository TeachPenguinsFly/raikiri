#pragma once

// Wire helpers specific to BitMEX; the shared readers live in raikiri::wire.

#include <raikiri/venues/bitmex/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitmex::detail {
using namespace raikiri::wire;

// Reads a JSON NUMBER as the digits BitMEX wrote, so parse_decimal_e9 sees
// "0.000000001" rather than whatever a double rounds it to. An absent field or
// a JSON null yields empty, which every caller reads as "not in this frame".
[[nodiscard]] std::expected<std::string_view, WireError> opt_num(ondemand::object& obj,
                                                                 std::string_view key);

// Every websocket table wraps its rows in `data`, and the symbol-scoped
// subscriptions this adapter opens put exactly one row in the frames that are
// read as a single value.
template <typename Fn>
[[nodiscard]] auto with_first_data(ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<ondemand::object&>())) {
    auto array = req_array(root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        return std::forward<Fn>(fn)(obj);
    }
    // An empty data array carries no datum; reporting one would invent it.
    return std::unexpected(WireError::MissingField);
}

} // namespace raikiri::venues::bitmex::detail
