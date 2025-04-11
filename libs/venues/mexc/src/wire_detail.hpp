#pragma once

// Plumbing shared by the mexc wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so that it
// takes the shape of all of them rather than the shape of whichever one was
// written first. What differs already is visible below — MEXC quotes prices,
// sizes and timestamps as JSON numbers, so the scalar reader here returns the
// number TOKEN rather than the contents of a string.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/mexc/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::mexc::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// MEXC made it, so the frame is copied once into a padded scratch buffer that
// lives for the life of the thread, and every view simdjson produces is
// translated back to the caller's buffer (rebase) — nothing handed out points
// into scratch memory the next frame overwrites.
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

// Reads any scalar as a view over the caller's buffer: a number keeps its
// token, a string loses its quotes, JSON null reads as empty. One reader for
// both because MEXC mixes the two encodings for the same kind of value —
// push.deal's `t` is a number and its `cts` the same instant quoted.
[[nodiscard]] std::expected<std::string_view, WireError> scalar_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_scalar(ondemand::object& obj,
                                                                    std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_scalar(ondemand::object& obj,
                                                                    std::string_view key);

// Signed because the scale and state fields are absent-or-negative sentinels
// in the DTOs, and because the depth version chain is compared as a number.
[[nodiscard]] std::expected<std::int64_t, WireError>
opt_i64(ondemand::object& obj, std::string_view key, std::int64_t fallback);

[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// Epoch milliseconds must not go through parse_decimal_e9: that scales by 1e9
// and an epoch-ms value overflows int64 on the way. Anything that is not
// exactly a run of digits yields 0, which the mapping layer reads as "venue
// gave us no time" and substitutes ingest time.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
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

} // namespace raikiri::venues::mexc::detail
