#pragma once

// What the three hyperliquid clients share, and nothing else.
//
// The public socket, the private socket and the catalogue are separate
// concerns that happen to run against one connection state. Only that state
// crosses the boundary; each client keeps its own constants and its own
// helpers in its own translation unit, where a reader looking for one of them
// is not made to scroll past the other two.
//
// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/hyperliquid/errors.hpp>
#include <raikiri/venues/hyperliquid/exchange.hpp>
#include <raikiri/venues/hyperliquid/map.hpp>
#include <raikiri/venues/hyperliquid/rest.hpp>
#include <raikiri/venues/hyperliquid/ws.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raikiri::venues::hyperliquid::detail {

// Owned as strings: the instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string coin;
};

// State one gateway process shares. Single threaded by contract: the gateway
// loop is the only caller, which is why nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // The wallet the account reads name. Owned as a string because the request
    // builders take views into it, and it is NOT a secret: it travels in a
    // plain unsigned /info body, which is the whole reason those reads work
    // with no key material at all.
    std::string address;

    // InstrumentId -> the asset id an /exchange action names. Built at
    // catalogue load because it is the one number an instrument cannot yield:
    // SymbolId counts the rows this adapter KEPT, and the venue counts the rows
    // it PUBLISHED, delisted ones included.
    std::unordered_map<std::uint64_t, std::int64_t> asset_ids;

    // The secp256k1 key orders are signed with, and the only place raw key
    // material lives in this adapter. Absent for a market-data-only gateway,
    // which is why every build path checks before it signs.
    Wallet wallet;

    // Strictly increasing across the process, because the venue rejects a
    // nonce at or below the smallest of the hundred highest it has seen.
    NonceSource nonces;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    std::uint64_t subscribe_refusals{0};

    void add_subscription(std::string_view channel, std::string_view coin) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.coin == coin) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{coin});
    }
};

// Cold paths only: catalogue loads and reconciliation pulls, never an order.
inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    std::optional<net::RestResponse> result;
    if (!rest.send(request,
                   [&result](net::RestResponse&& response) { result = std::move(response); })) {
        return std::nullopt;
    }
    const auto deadline = state.clock->mono().ns + timeout_ms * 1'000'000ULL;
    while (!result.has_value() && state.clock->mono().ns < deadline) {
        if (state.io.poll() == 0) {
            state.io.run_for(5);
        }
    }
    return result;
}

// A query this venue cannot satisfy comes back HTTP 200 with a body of the bare
// literal `null`, so the status alone cannot tell a refusal from a real read —
// and a caller that trusted it would parse an empty account.
inline std::expected<std::string, exec::VenueError> read_info(VenueState& state,
                                                              net::RestClient& rest,
                                                              const net::RestRequest& request,
                                                              std::uint64_t timeout_ms = 10'000) {
    if (request.body.empty()) {
        // The builder reports overflow by emitting nothing: a truncated body
        // would be a DIFFERENT query, answered rather than refused.
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Terminal, 0, "hyperliquid: request body did not fit")};
    }
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Connectivity, 0, "hyperliquid: no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const bool ok_status = response->status >= 200 && response->status < 300;
    if (!ok_status || is_null_body(response->body)) {
        const auto reason = is_null_body(response->body) ? std::string_view{"/info answered null"}
                                                         : std::string_view{response->body};
        return std::unexpected{exec::VenueError::make(
            classify(response->status, response->body), response->status, reason)};
    }
    return response->body;
}

inline exec::VenueError unreadable(WireError err) noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

} // namespace raikiri::venues::hyperliquid::detail
