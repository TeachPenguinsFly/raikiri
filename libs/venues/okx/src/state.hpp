#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/venues/okx/map.hpp>
#include <raikiri/venues/okx/rest.hpp>
#include <raikiri/venues/okx/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::okx::detail {

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
};

// State every okx connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;
    std::uint64_t order_answer_timeout_ns{0};

    // What each instrument's two funding channels have said so far. OKX splits
    // the rate and the mark across them and the record is written whole — see
    // market_data.hpp.
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    net::IoContext io;
    net::Signer signer;
    config::Secret passphrase;
    exec::InstrumentTable instruments;
    Protocol protocol;
    // The private socket builds its frames from the same class; giving it its
    // own instance keeps one socket's scratch buffer out of the other's.
    Protocol private_protocol;
    RequestBuilder builder;

    // Which markets this process was configured for. The private order
    // channel subscribes by instrument type, so it needs to be told.
    bool spot_enabled{false};
    bool perp_enabled{true};

    std::vector<OwnedSubscription> desired;

    std::unordered_map<InstrumentId, Book> books;
    std::uint64_t next_app_ping_ns{0};

    // Venue clock offset, measured against /api/v5/public/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    [[nodiscard]] bool credentialed() const noexcept {
        return signer.has_credentials() && !passphrase.empty();
    }

    // OKX refuses a signature more than 30 s from ITS clock (50102), so a
    // laptop drifting a minute looks exactly like a bad key.
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{inst_id});
    }

    Book& book_for(const Instrument& instrument) {
        const auto& base = base_of(instrument);
        const auto it = books.find(base.id);
        if (it != books.end()) {
            return it->second;
        }
        return books.emplace(base.id, Book{base.id, base.tick_size, base.lot_size}).first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::okx::detail
