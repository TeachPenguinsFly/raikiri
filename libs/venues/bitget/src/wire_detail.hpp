#pragma once

// Wire helpers specific to bitget; the shared readers live in raikiri::wire.

#include <raikiri/venues/bitget/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget::detail {
using namespace raikiri::wire;

// Both transports wrap the payload in a `data` array and every channel plus
// every singleton REST read carries one element. The orderbook is the
// exception and does not use this.
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
    return std::unexpected(WireError::MissingField);
}

// Shared by the books channels and the REST orderbook, whose level arrays are
// identical even though their envelopes are not.
[[nodiscard]] std::expected<void, WireError>
read_book_body(ondemand::object& obj, LevelBuffer& storage, WireBooks& out);
} // namespace raikiri::venues::bitget::detail
