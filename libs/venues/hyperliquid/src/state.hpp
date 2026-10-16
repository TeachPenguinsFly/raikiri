#pragma once

// `internal` because an anonymous namespace in a header makes a distinct type per includer

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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

namespace raikiri::venues::hyperliquid::internal {

struct OwnedSubscription {
    std::string channel;
    std::string coin;
};

// single threaded; only the gateway loop calls in
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    // set for an account poller: no sockets are opened
    bool pulls_only{false};
    bool testnet;
    MarketType market{MarketType::Perp};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // the account address; public, sent in unsigned /info bodies
    std::string address;

    // instrument id to /exchange asset id; the venue counts delisted rows too
    std::unordered_map<std::uint64_t, std::int64_t> asset_ids;

    // hip-3 dexes loaded beside the main dex; "all" takes every one perpDexs lists
    std::vector<std::string> dexes;
    // the hip-3 dexes the catalogue resolved; account reads ask each beside the main dex
    std::vector<std::string> loaded_dexes;

    // absent on a market-data-only gateway
    Wallet wallet;

    NonceSource nonces;

    // read once at start; without them a fill from an /exchange answer books no fee
    std::optional<FeeRates> fee_rates;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    std::uint64_t subscribe_refusals{0};

    // instrument to the ms its trades ack arrived, until the coin's first push settles the backfill
    std::unordered_map<std::uint64_t, std::uint64_t> trade_ack_ms;

    // false when already asked, since funding and mark price share activeAssetCtx
    [[nodiscard]] bool add_subscription(std::string_view channel, std::string_view coin) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.coin == coin) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{coin});
        return true;
    }
};

// cold paths only, never an order
inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// a 200 with a `null` body is a refusal
inline std::expected<std::string, exec::VenueError> read_info(VenueState& state,
                                                              net::RestClient& rest,
                                                              const net::RestRequest& request,
                                                              std::uint64_t timeout_ms = 10'000) {
    if (request.body.empty()) {
        // an empty body means the builder overflowed
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, "hyperliquid: request body did not fit")};
    }
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "hyperliquid: no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const bool ok_status = response->status >= 200 && response->status < 300;
    if (!ok_status || is_null_body(response->body)) {
        const auto reason = is_null_body(response->body) ? std::string_view{"/info answered null"}
                                                         : std::string_view{response->body};
        return std::unexpected{
            exec::VenueError::make(classify(response->status, response->body), response->status, reason)};
    }
    return response->body;
}

inline exec::VenueError unreadable(WireError err) noexcept {
    return exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(err));
}

} // namespace raikiri::venues::hyperliquid::internal
