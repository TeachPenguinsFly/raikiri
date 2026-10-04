#pragma once

// split from json.hpp so naming a failure does not pull in simdjson

#include <cstdint>
#include <string_view>

namespace raikiri::wire {

enum class Error : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
    UnknownMessage, // a topic, stream or event this adapter does not read
    ShortArray,     // a positional frame shorter than its declared schema
};

[[nodiscard]] std::string_view to_string(Error err) noexcept;

// frames simdjson found broken, counted once per frame across every thread
[[nodiscard]] std::uint64_t broken_frames() noexcept;

} // namespace raikiri::wire
