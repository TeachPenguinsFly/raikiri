#pragma once

// internal namespace: an anonymous namespace in a header is a different type per file

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/errors.hpp>
#include <raikiri/venues/aster/map.hpp>
#include <raikiri/venues/aster/rest.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::aster::internal {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // shared with the account poller: aster meters one weight pool per ip
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder{signer};

    // own pool so seeds and orders never queue behind each other; six connections for a full-catalogue seed
    net::RestClient depth_rest{
        io, net::RestConfig{.host = std::string{rest_host(testnet)}, .pool_size = 6}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<std::string> desired_streams;

    // funding is polled; @markPrice never sends on this venue
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // book seeds are rest calls paced by drain_seeds and charged by meter_market_data
    std::vector<InstrumentId> pending_seeds;
    std::uint64_t next_seed_ns{0};

    // venue clock offset, measured against /fapi/v1/time
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

    // private frames dropped before reaching the book
    std::uint64_t private_frames_dropped{0};

    std::function<void(UnixNanos)> tick_hook;

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

// market-data rest spends the venue's limits table as a poll; costs come from config
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::aster::internal
