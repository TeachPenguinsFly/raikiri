#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
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
#include <vector>

namespace raikiri::venues::bitget::detail {

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
};

// State every bitget connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref,
               const Clock& clock_ref,
               bool testnet_flag,
               MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};
    // One process serves one market: the table interns by exchange symbol and
    // BTCUSDT names both a spot pair and a perpetual, so loading both
    // catalogues into one table would drop half of the second one.
    MarketType market;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;

    net::Signer signer;
    // Bitget authenticates with three pieces, not two. The passphrase lives
    // in the same locked, zeroizing storage as the secret and is only ever
    // viewed, never copied into a header the adapter owns.
    config::Secret passphrase;

    std::vector<OwnedSubscription> desired;

    std::uint64_t next_app_ping_ns{0};

    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    // Bitget refuses a signature more than 30 s from ITS clock, so a laptop
    // that drifts looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    [[nodiscard]] std::string_view product() const noexcept { return product_type(testnet); }

    // True when the pair was new.
    bool add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{inst_id});
        return true;
    }
};

// Cold-path request/response. Every caller is a startup or reconciliation
// path, never the hot loop, so pumping the reactor until the answer arrives is
// exactly right: a gateway that trades before it knows its tick sizes is worse
// than one that takes an extra second to boot.
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

// The response body of a checked call. An empty result and a refused call
// must never be told apart downstream by an empty vector.
inline std::expected<std::string, exec::VenueError> fetch_checked(VenueState& state,
                                                                  net::RestClient& rest,
                                                                  const net::RestRequest& request,
                                                                  std::uint64_t timeout_ms) {
    const auto response = await_response(state, rest, request, timeout_ms);
    if (!response.has_value()) {
        return std::unexpected{exec::VenueError::make(
            exec::ErrorClass::Connectivity, 0, "no response before deadline")};
    }
    if (response->transport_failed()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Connectivity, 0, response->error)};
    }
    const auto envelope = parse_rest_envelope(response->body);
    if (!envelope.has_value()) {
        return std::unexpected{
            exec::VenueError::make(exec::ErrorClass::Terminal, 0, to_string(envelope.error()))};
    }
    if (envelope->code != k_ok) {
        return std::unexpected{exec::VenueError::make(
            classify(envelope->code), code_number(envelope->code), envelope->msg)};
    }
    return response->body;
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::bitget::detail
