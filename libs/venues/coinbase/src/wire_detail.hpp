#pragma once

// Plumbing shared by the coinbase wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so that it
// takes the shape of all of them rather than the shape of whichever one was
// written first. What differs already is visible below — Coinbase has no
// digit-string numbers at all, so there is no digits_to_u64 here, and it does
// carry real JSON booleans in the product catalogue, which okx does not.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/coinbase/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::coinbase::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// Coinbase made it, so the frame is copied once into a padded scratch buffer
// that lives for the life of the thread, and every view simdjson produces is
// translated back to the caller's buffer (rebase) — nothing handed out points
// into scratch memory the next frame overwrites.
class Session {
public:
    [[nodiscard]] std::expected<ondemand::object, WireError> open_object(std::string_view frame);

    [[nodiscard]] std::expected<ondemand::array, WireError> open_array(std::string_view frame);

    [[nodiscard]] std::string_view rebase(std::string_view scratch_view) const noexcept;

private:
    [[nodiscard]] std::expected<void, WireError> load(std::string_view frame);

    simdjson::padded_string scratch_;
    ondemand::parser parser_;
    ondemand::document doc_;
    const char* origin_{nullptr};
    std::size_t origin_size_{0};
};

// One session per thread: rebuilding the structural index allocation per
// frame is the largest avoidable cost simdjson has, and a gateway runs one
// venue connection per thread.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// `sequence` and `trade_id` are JSON numbers here, not the digit strings okx
// uses. An absent field yields 0.
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

// A missing flag reads as false: the catalogue's booleans are all
// restrictions, so absence means unrestricted.
[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
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

} // namespace raikiri::venues::coinbase::detail
