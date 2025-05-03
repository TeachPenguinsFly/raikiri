#pragma once

// Wire helpers specific to hyperliquid; the shared readers live in raikiri::wire.

#include <raikiri/venues/hyperliquid/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::hyperliquid::detail {
using namespace raikiri::wire;

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

} // namespace raikiri::venues::hyperliquid::detail
