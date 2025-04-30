#pragma once

// Plumbing shared by the xt wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so that it
// takes the shape of all of them rather than the shape of whichever one was
// written first. What differs already is visible below — XT puts its
// timestamps in JSON numbers and its sequence ids in digit strings, so both a
// numeric and a textual reader are needed where okx needed only the textual
// one.

#include <raikiri/venues/xt/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::xt::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as XT
// made it, so the frame is copied once into a padded scratch buffer that lives
// for the life of the thread, and every view simdjson produces is translated
// back to the caller's buffer (rebase) — nothing handed out points into
// scratch memory the next frame overwrites.
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
// is the largest avoidable cost simdjson has, and a gateway runs one venue
// connection per thread.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// Raw JSON text of a field, whatever its type. The only way to read a number
// without letting simdjson choose a binary representation for it.
[[nodiscard]] std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<std::int64_t, WireError> req_i64(ondemand::object& obj,
                                                             std::string_view key);
[[nodiscard]] std::expected<bool, WireError> req_bool(ondemand::object& obj, std::string_view key);

// XT's `t` is epoch milliseconds as a JSON number. Reading it as a number
// keeps it away from parse_decimal_e9, which scales by 1e9 and overflows int64
// on an epoch-ms value.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

// Sequence ids (`u`, `pu`, `fu`, `id`) are digit STRINGS. Anything that is not
// exactly a run of digits yields 0.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> req_digits(ondemand::object& obj,
                                                                 std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
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

} // namespace raikiri::venues::xt::detail
