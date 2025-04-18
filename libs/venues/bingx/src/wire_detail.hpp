#pragma once

// Plumbing shared by the bingx wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so that it takes
// the shape of all of them rather than of whichever was written first. What
// differs already is below — BingX puts its timestamps and codes in JSON
// numbers, so okx's digit-string readers have no counterpart here.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/bingx/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::bingx::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and needs that many readable
// bytes past the document, which an inflated frame does not have. The frame is
// copied once into a thread-lifetime padded buffer and every view simdjson
// produces is translated back to the caller's (rebase) — nothing handed out
// points into scratch the next frame overwrites.
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

// One session per thread: rebuilding the structural index allocation per frame
// is the largest avoidable cost simdjson has.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// BingX epoch milliseconds, ids, codes and precisions are all JSON numbers. An
// absent field yields 0, which callers read as "the venue said nothing".
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

// For the numbers where zero is a meaningful value rather than an absence: a
// missing `code` must not read as success, and a missing pricePrecision must
// not read as a tick size of 1.
[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj,
                                                             std::string_view key);

// Raw JSON text of a field, whatever its type — the only way to carry a number
// BingX may have written in scientific notation.
[[nodiscard]] std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// "BTC-USDT@depth5@100ms" -> "BTC-USDT". The channel suffix may itself contain
// '@', so the split is on the FIRST one. Empty when there is no '@' at all.
// Not noexcept only because substr is not; every caller is inside guarded().
[[nodiscard]] std::string_view symbol_of(std::string_view data_type);

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

} // namespace raikiri::venues::bingx::detail
