#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/binance/book.hpp>
#include <raikiri/venues/binance/endpoints.hpp>
#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/venues/binance/map.hpp>
#include <raikiri/venues/binance/rest.hpp>
#include <raikiri/venues/binance/ws.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::binance::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // Shared with the account poller: binance counts one pool of weight per IP,
    // so two private buckets would together spend twice the allowance.
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder{signer};

    // The depth seed rides its own pool: a book resynchronising must never
    // queue behind an order, and an order must never queue behind a 40 kB
    // snapshot body.
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet)}}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<std::string> desired_streams;

    // Instruments the gateway asked funding for. The rate is PULLED, not
    // streamed: @markPrice is acknowledged on this venue and then never sends
    // a frame, so a gateway that trusted the stream carried no funding at all.
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // Venue clock offset, measured against /fapi/v1/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

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

} // namespace raikiri::venues::binance::detail
