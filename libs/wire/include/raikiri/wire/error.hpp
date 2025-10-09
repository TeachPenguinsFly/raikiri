#pragma once

// The parse-failure vocabulary, split out of json.hpp so that naming a
// failure does not compile a JSON parser.
//
// Almost every venue public header wants this enum and nothing else from
// json.hpp, and json.hpp opens with <simdjson.h> — a 19k-line amalgamation
// that drags <iostream>, <locale>, <sstream> and <set> in behind it. Include
// this header from anything that only classifies; include json.hpp only where
// simdjson types are actually named.

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

} // namespace raikiri::wire
