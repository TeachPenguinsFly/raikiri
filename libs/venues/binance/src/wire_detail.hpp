#pragma once

// Wire helpers specific to binance; the shared readers live in raikiri::wire.

#include <raikiri/venues/binance/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance::detail {
using namespace raikiri::wire;

// A level is a two-element array of strings, ["64488.50","5.560"], on the
// depth stream and in the REST snapshot alike.
[[nodiscard]] inline std::expected<std::size_t, Error>
read_levels(ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(Error::TooManyEntries);
        }
        ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        WireLevel level{};
        std::size_t field = 0;
        for (auto element : pair) {
            ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(map_error(ec));
            }
            const auto text = str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(Error::MissingField);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

} // namespace raikiri::venues::binance::detail
