#pragma once

// Plumbing shared by the hyperliquid wire parsers.
//
// Close to a copy of the okx equivalent, deliberately: the shared layer gets
// extracted once there are enough adapters to extract it FROM, so it takes the
// shape of all of them rather than of whichever was written first. What differs
// is visible below — timestamps in JSON NUMBERS, and no numeric error codes.
// simdjson stays confined here; wire.hpp hands out only string_view and span.

#include <raikiri/venues/hyperliquid/wire.hpp>

#include <simdjson.h>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::hyperliquid::detail {

namespace ondemand = simdjson::ondemand;

[[nodiscard]] WireError map_error(simdjson::error_code ec) noexcept;

// On-Demand needs SIMDJSON_PADDING readable bytes past the document, so a
// frame is copied once into a padded thread-lifetime scratch buffer and every
// view simdjson produces is translated back to the caller's buffer (rebase) —
// nothing handed out points into scratch the next frame overwrites.
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

// One per thread: rebuilding the structural index allocation per frame is the
// largest avoidable cost simdjson has.
[[nodiscard]] Session& session() noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// JSON null reads as empty: a delisted asset's midPx, and an empty book side.
[[nodiscard]] std::expected<std::string_view, WireError> str_of(ondemand::value value);

[[nodiscard]] std::expected<std::string_view, WireError> opt_str(ondemand::object& obj,
                                                                 std::string_view key);
[[nodiscard]] std::expected<std::string_view, WireError> req_str(ondemand::object& obj,
                                                                 std::string_view key);

// Raw text of a field: the only way to keep a JSON NUMBER as a caller view.
[[nodiscard]] std::expected<std::string_view, WireError> opt_raw(ondemand::object& obj,
                                                                 std::string_view key);

// szDecimals and maxLeverage are JSON numbers; an absent field reads as 0.
[[nodiscard]] std::expected<std::int64_t, WireError> opt_i64(ondemand::object& obj,
                                                             std::string_view key);

// isDelisted is ABSENT on a live asset rather than present-and-false.
[[nodiscard]] std::expected<bool, WireError> opt_bool(ondemand::object& obj, std::string_view key);

[[nodiscard]] std::expected<ondemand::array, WireError> req_array(ondemand::object& obj,
                                                                  std::string_view key);
[[nodiscard]] std::expected<ondemand::object, WireError> req_object(ondemand::object& obj,
                                                                    std::string_view key);

// Must not go through parse_decimal_e9: that scales by 1e9 and an epoch-ms
// value overflows int64 on the way. A non-digit run yields 0, which the mapper
// reads as "no venue time" and replaces with ingest time.
[[nodiscard]] std::uint64_t digits_to_u64(std::string_view s) noexcept;

// Reads epoch milliseconds out of a JSON NUMBER. An absent field yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj,
                                                             std::string_view key);

// Shared because the venue ships the identical object on both transports:
// inside `data.ctx` on activeAssetCtx, and as a bare row in metaAndAssetCtxs.
[[nodiscard]] std::expected<void, WireError> read_ctx_fields(ondemand::object& ctx,
                                                             WireAssetCtx& out);

// Every payload hangs off `data`; every channel but `trades` puts an object.
template <typename Fn>
[[nodiscard]] auto with_data_object(ondemand::object& root, Fn&& fn)
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

} // namespace raikiri::venues::hyperliquid::detail
