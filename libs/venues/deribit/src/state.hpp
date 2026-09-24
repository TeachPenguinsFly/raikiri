#pragma once

// internal namespace: an anonymous one in a header is a distinct type per includer

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/venues/deribit/rest.hpp>
#include <raikiri/venues/deribit/ws.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::deribit::internal {

constexpr std::uint32_t k_heartbeat_interval_s = 15;

struct OwnedSubscription {
    std::string channel;
};

// single threaded by contract
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    Protocol private_protocol; // its own id sequence
    RequestBuilder builder;
    net::Signer signer;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_requested{0};
    std::uint64_t channels_accepted{0};
    std::int64_t last_error_code{0};

    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = static_cast<std::int64_t>(clock->wall().ns);
        return UnixNanos{static_cast<std::uint64_t>(local + clock_offset_ns)};
    }

    void add_subscription(std::string_view channel) {
        for (const auto& existing : desired) {
            if (existing.channel == channel) {
                return; // refcounting lives in the gateway core
            }
        }
        desired.emplace_back(std::string{channel});
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

void sync_clock(VenueState& state, net::RestClient& rest);

// true for a test_request; the caller answers on the socket that asked
[[nodiscard]] bool handle_frame(VenueState& state, std::string_view frame, UnixNanos received);

} // namespace raikiri::venues::deribit::internal
