#pragma once

// Wire helpers specific to OKX; the shared readers live in raikiri::wire.

#include <raikiri/venues/okx/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx::detail {
using namespace raikiri::wire;

// Both transports wrap the payload in a `data` array, and every channel here
// plus every singleton REST read carries exactly one element.
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
} // namespace raikiri::venues::okx::detail
