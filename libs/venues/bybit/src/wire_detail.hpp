#pragma once

// Plumbing shared by the bybit wire parsers.
//
// simdjson is deliberately confined to this directory. wire.hpp hands out
// std::string_view and std::span only, so the JSON library is a swappable
// implementation detail rather than something every caller compiles against.

#include <raikiri/venues/bybit/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::bybit::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// simdjson On-Demand reads the input in SIMDJSON_PADDING-byte blocks and so
// requires that many readable bytes past the end of the document. A websocket
// frame is exactly as long as bybit made it, and forcing every caller to
// over-allocate its read buffer would drag simdjson back into the public
// contract. The frame is therefore copied once into a padded scratch buffer
// that lives for the life of the thread — one memcpy of a few kilobytes,
// against a parse that touches every one of those bytes anyway.
//
// The copy is byte-identical to the input, so every view simdjson produces is
// translated back into the equivalent view over the caller's buffer
// (rebase()). That is what keeps the zero-copy promise honest: nothing handed
// back points into scratch memory that the next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One session per thread rather than per call: the parser owns the structural
// index, and rebuilding that allocation per frame is the largest avoidable
// cost simdjson has. A gateway runs one venue connection per thread, which
// makes "thread_local" and "per connection" the same statement.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads one JSON string value as a view over the caller's buffer. JSON null
// reads as empty, which is how bybit spells "no value here" in the fields it
// does bother to send.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

// Absent field yields an empty view, not an error. Bybit's `tickers` stream
// sends one snapshot and then deltas carrying only what changed, so "not
// present" has to mean "unchanged"; treating it as zero silently wipes a
// funding rate the strategy is pricing against. Present-but-wrong-type is a
// different animal — that is a schema change and it fails loudly.
[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);

// Symbol identity is the one string we refuse to guess at, because a frame
// applied to the wrong instrument is worse than a frame dropped.
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

// Raw JSON text of a field, whatever its type — the REST envelope's "result"
// is handed to a second parser rather than decoded here.
[[nodiscard]] std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] bool has_field(ondemand::object& obj, std::string_view key);

// Required array field: the shape of `data` is structure, not content.
[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// Bybit encodes its private-stream timestamps as milliseconds inside a JSON
// string. Anything that is not exactly a run of digits yields 0, which the
// mapping layer reads as "venue gave us no time" and substitutes ingest time.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

// simdjson's On-Demand API throws whenever a simdjson_result is used through
// one of its implicit conversions. This file never uses those — every access
// goes through .get()/.error() — but "never" is a claim about code that other
// people will edit. The catch is the cheap half of that argument, and an
// exception escaping into a gateway that holds live orders is the expensive
// half.
template <typename Fn> [[nodiscard]] auto guarded(Fn&& fn) -> decltype(fn()) {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return std::unexpected(WireError::NotJson);
    }
}

} // namespace raikiri::venues::bybit::detail
