#pragma once

// What every Hyperliquid answer is wrapped in.
//
// A socket frame names its channel and hangs its payload off `data`. REST has
// NO envelope at all: /info is one POST endpoint whose answer takes whatever
// shape the request type asks for — an object for clearinghouseState, a bare
// array for openOrders, a two-element array for metaAndAssetCtxs — so the
// module that owns each request owns the reading of it.

#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace raikiri::venues::hyperliquid {

// From `channel` once, so dispatch is an integer rather than a string compare.
enum class EventKind : std::uint8_t {
    Bbo,      // bbo — tick-by-tick top of book
    Trade,    // trades
    Book,     // l2Book — full depth SNAPSHOTS, never deltas
    AssetCtx, // activeAssetCtx and activeSpotAssetCtx — mark, funding, turnover
    SubscribeAck,
    Pong,
    Error,
    Unknown,
};

// No coin here: it sits inside `data` at a different depth per channel
// (`data.coin`, but `data[0].coin` on trades), so each parser reads its own.
[[nodiscard]] std::expected<EventKind, WireError> parse_event(std::string_view frame);

// {"channel":"error","data":"<prose>"} — `data` is a plain STRING and there is
// no numeric code anywhere in the protocol, so unlike okx's 60018 there is
// nothing to classify on but text. VERIFIED live: the venue also CLOSES the
// socket, so a bad subscription costs every other one on that connection.
struct StreamError {
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// Epoch milliseconds out of a JSON NUMBER. Named apart from wire::opt_ms
// because it is not the same reader: that one reads a digit STRING, which is
// what most venues send and what this one never does. An absent field yields 0.
[[nodiscard]] std::expected<std::uint64_t, WireError> opt_number_ms(wire::ondemand::object& obj,
                                                                    std::string_view key);

// Every socket payload hangs off `data`; every channel but `trades` puts an
// object there.
template <typename Fn>
[[nodiscard]] auto with_data_object(wire::ondemand::object& root, Fn&& fn)
    -> decltype(fn(std::declval<wire::ondemand::object&>())) {
    auto obj = wire::req_object(root, "data");
    if (!obj.has_value()) {
        return std::unexpected(obj.error());
    }
    return std::forward<Fn>(fn)(*obj);
}

} // namespace raikiri::venues::hyperliquid
