#pragma once

// internal namespace: an anonymous one in a header is a distinct type per file

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/venues/bitget/errors.hpp>
#include <raikiri/venues/bitget/map.hpp>
#include <raikiri/venues/bitget/rest.hpp>
#include <raikiri/venues/bitget/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bitget::internal {

struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
};

// single threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};
    // one process serves one market; BTCUSDT names both a spot pair and a perpetual
    MarketType market;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;
    // the passphrase stays in locked secret storage and is only viewed
    config::Secret passphrase;

    std::vector<OwnedSubscription> desired;

    exec::RateBudget* budget{nullptr};
    std::unordered_map<InstrumentId, FundingClock> funding_clocks;
    // venue ms of each quoted instrument's last books1 push; 0 until the first
    std::unordered_map<InstrumentId, std::uint64_t> quoted_ms;

    std::uint64_t next_app_ping_ns{0};

    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // bitget refuses a signature more than 30 s from its clock
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    [[nodiscard]] std::string_view product() const noexcept { return product_type(testnet); }

    // true when the pair was new
    bool add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return false; // refcounting lives in the gateway core
            }
        }
        desired.emplace_back(std::string{channel}, std::string{inst_id});
        return true;
    }
};

// cold path only: startup and reconciliation pump the reactor until answered
inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// keeps an empty result distinguishable from a refused call
inline std::expected<std::string, exec::VenueError>
fetch_checked(VenueState& state, net::RestClient& rest, const net::RestRequest& request, std::uint64_t timeout_ms) {
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, "no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const auto envelope = parse_rest_envelope(response->body);
    if (!envelope.has_value()) {
        return std::unexpected{exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
    }
    if (envelope->code != k_ok) {
        return std::unexpected{
            exec::VenueError::make(classify(envelope->code), code_number(envelope->code), envelope->msg)};
    }
    return response->body;
}

// a skewed clock reads to the venue as a bad signature

} // namespace raikiri::venues::bitget::internal
