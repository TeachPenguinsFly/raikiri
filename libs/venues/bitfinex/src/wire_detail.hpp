#pragma once

// Wire helpers specific to Bitfinex; the shared readers live in raikiri::wire.

#include <raikiri/venues/bitfinex/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <array>

namespace raikiri::venues::bitfinex::detail {
using namespace raikiri::wire;

// Which document root a frame has, decided from its first non-space byte.
// Cheaper and simpler than opening the document and asking its type, and the
// two shapes are the whole of Bitfinex's dispatch: `{` is a control frame,
// `[` is data.
[[nodiscard]] bool is_array_frame(std::string_view frame) noexcept;

// Raw JSON token of a value, as a view over the CALLER's buffer. Strings lose
// their quotes; `null` reads as empty.
//
// Numbers keep their exact spelling, scientific notation included — map.cpp's
// exponent-aware parser needs the token, and get_string() would move the bytes
// into simdjson's unescape buffer, which cannot be rebased. That lifetime bug
// has already cost one debug cycle in this repo.
[[nodiscard]] std::expected<std::string_view, WireError> token_of(ondemand::value value);

// A positional row, read into a fixed number of raw tokens.
//
// Arity is a TEMPLATE parameter, not a runtime count, so at<I>() past the
// schema a caller declared is a compile error rather than a plausible number.
template <std::size_t Arity> struct Row {
    std::array<std::string_view, Arity> fields{};

    template <std::size_t Index> [[nodiscard]] std::string_view at() const noexcept {
        static_assert(Index < Arity, "positional read past the schema's declared arity");
        return fields[Index];
    }
};

// Reads the first `Arity` elements of a JSON array.
//
// A shorter array is ShortArray, never a partly-filled Row: on a positional
// wire, truncation means the reader cannot tell which field it stopped in the
// middle of. Extra trailing elements are ignored on purpose — Bitfinex already
// sends eleven ticker fields where it documents ten, and the `conf` TIMESTAMP
// flag appends to the tail of every envelope.
template <std::size_t Arity>
[[nodiscard]] std::expected<Row<Arity>, WireError> read_row(ondemand::array array) {
    Row<Arity> out{};
    std::size_t index = 0;
    for (auto entry : array) {
        if (index == Arity) {
            break;
        }
        ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(map_error(ec));
        }
        const auto token = token_of(value);
        if (!token.has_value()) {
            return std::unexpected(token.error());
        }
        out.fields[index] = *token;
        ++index;
    }
    if (index < Arity) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

template <std::size_t Arity>
[[nodiscard]] std::expected<Row<Arity>, WireError> read_row(ondemand::value value) {
    ondemand::array array;
    if (const auto ec = value.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(map_error(ec));
    }
    return read_row<Arity>(array);
}

// Signed variant for codes and level counts. Returns false rather than a
// plausible zero when the token is not an integer.
[[nodiscard]] bool token_to_i64(std::string_view s, std::int64_t& out) noexcept;

// Object-side reads. Only control frames need these, and only for the handful
// of fields Bitfinex does name.
[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);

} // namespace raikiri::venues::bitfinex::detail
