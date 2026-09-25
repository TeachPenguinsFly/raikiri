#pragma once

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::hyperliquid {

enum class EventKind : std::uint8_t {
    Bbo, // tick-by-tick top of book
    Trade,
    Book,     // full snapshots
    AssetCtx, // activeAssetCtx and activeSpotAssetCtx: mark, funding, turnover
    SubscribeAck,
    Pong,
    Error,
    Unknown,
};

// the coin sits at a different depth per channel, so each parser reads its own
[[nodiscard]] std::expected<EventKind, WireError> parse_event(std::string_view frame);

// `data` is prose with no code, and the venue also closes the socket
struct StreamError {
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// reads a json number, unlike wire::opt_ms which reads a digit string; absent yields 0
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_number_ms(wire::ondemand::object& obj, std::string_view key);

template <typename Fn>
[[nodiscard]] auto with_data_object(wire::ondemand::object& root,
                                    Fn&& fn) -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

} // namespace raikiri::venues::hyperliquid
