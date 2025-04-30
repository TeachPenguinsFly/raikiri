#pragma once

// Plumbing shared by the bitfinex wire parsers.
//
// The Session/rebase half is a deliberate copy of the okx and binance
// equivalents; the shared layer gets extracted once there are enough adapters
// to extract it FROM, so that it takes the shape of all of them rather than
// the shape of whichever one was written first.

#include <raikiri/venues/bitfinex/wire.hpp>

#include <simdjson.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::bitfinex::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// Bitfinex made it, so the frame is copied once into a padded scratch buffer
// that lives for the life of the thread, and every view simdjson produces is
// translated back to the caller's buffer (rebase) — nothing handed out points
// into scratch memory the next frame overwrites.
class Session {
public:
    // Control frames are objects and data frames are arrays, so unlike okx
    // both entry points are load-bearing rather than one being a REST detail.
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);
    [[nodiscard]] std::expected<ondemand::array, WireError> open_array(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    [[nodiscard]] std::expected<void, WireError> start(std::string_view frame);

    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One session per thread: rebuilding the structural index allocation per frame
// is the largest avoidable cost simdjson has, and a gateway runs one venue
// connection per thread.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

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

// Bitfinex sends epoch milliseconds, sequence numbers and error codes as bare
// JSON numbers. These must not go through parse_decimal_e9: that scales by 1e9
// and an epoch-ms value overflows int64 on the way. Anything that is not
// exactly a run of digits yields 0, which the mapping layer reads as "venue
// gave us no time" and substitutes ingest time.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

// Signed variant for codes and level counts. Returns false rather than a
// plausible zero when the token is not an integer.
[[nodiscard]] bool token_to_i64(std::string_view s, std::int64_t& out) noexcept;

// Object-side reads. Only control frames need these, and only for the handful
// of fields Bitfinex does name.
[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

// simdjson's On-Demand API throws through its implicit conversions. This file
// never uses those, but "never" is a claim about code other people will edit,
// and an exception escaping into a gateway holding live orders is expensive.
template <typename Fn> [[nodiscard]] auto guarded(Fn&& fn) -> decltype(fn()) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return std::unexpected(WireError::NotJson);
    }
}

} // namespace raikiri::venues::bitfinex::detail
