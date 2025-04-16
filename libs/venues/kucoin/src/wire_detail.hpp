#pragma once

// Plumbing shared by the kucoin wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are three adapters to extract it FROM, so that it takes
// the shape of all of them. What differs already is visible below — KuCoin puts
// its timestamps and half its decimals in JSON numbers, so the scalar readers
// work from the raw token rather than from a string.
//
// simdjson stays confined to this directory; wire.hpp hands out only
// string_view and span.

#include <raikiri/venues/kucoin/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::kucoin::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand reads in SIMDJSON_PADDING-byte blocks and so needs that many
// readable bytes past the document. A websocket frame is exactly as long as
// KuCoin made it, so the frame is copied once into a padded scratch buffer
// that lives for the life of the thread, and every view simdjson produces is
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

[[nodiscard]] std::expected<std::string_view, WireError> raw_of(ondemand::value value);

// Reads a JSON string as a view over the caller's buffer; JSON null reads as
// empty.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// Numeric text with surrounding quotes removed, so that tickerV2's quoted prices
// and its bare sizes reach the DTO through one reader.
[[nodiscard]] std::expected<std::string_view, WireError> opt_decimal(ondemand::object& obj,
                                                                     std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_decimal(ondemand::object& obj,
                                                                     std::string_view key);

// Epoch and sequence fields. These must not go through parse_decimal_e9: it
// scales by 1e9 and a nanosecond epoch overflows int64 on the way. Anything
// that is not exactly a run of digits yields 0, which the mapping layer reads
// as "venue gave us no time" and substitutes ingest time.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

[[nodiscard]] std::expected<std::uint64_t, WireError> opt_u64(ondemand::object& obj,
                                                              std::string_view key);
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// Both transports wrap the payload in `data`, and unlike okx it is a single
// object on every channel and on every singleton REST read.
template <typename Fn>
[[nodiscard]] auto with_data(ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<ondemand::object&>())) {
    auto obj = req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

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

} // namespace raikiri::venues::kucoin::detail
