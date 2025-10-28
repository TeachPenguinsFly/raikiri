#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/weex/book.hpp>
#include <raikiri/venues/weex/rest.hpp>
#include <raikiri/venues/weex/ws.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace raikiri::venues::weex::detail {

// State every WEEX connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // ONE MARKET, and it is Perp. The contract tree lists nothing else and the
    // spot exchange is other hosts entirely, so this is a constant rather than
    // a choice — kept as a field because the id of every instrument carries it.
    MarketType market{MarketType::Perp};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;

    // The market-data builder, which signs nothing. The signed half holds its
    // own: one builder writes every request into one scratch buffer, so a
    // poller and an order path sharing one would overwrite each other's bytes
    // between the build and the send.
    RequestBuilder builder;

    // WEEX signs with THREE credentials, and the passphrase is as load-bearing
    // as the secret — without it the venue answers -1049, which is not the code
    // a missing key produces. It is a public value by construction (it travels
    // in a header on every request), which is why it may be held as a plain
    // string while the secret never leaves the signer.
    net::Signer signer;
    std::string passphrase;

    // WEEX refuses a signature stamped more than 30 SECONDS from its own clock,
    // and a laptop that has drifted wears the face of a revoked key. Measured
    // once at start against /capi/v3/market/time by the signed client; zero
    // until then, which is the honest reading of a clock nobody has compared.
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    // The catalogue and the funding poll share one client: both are cold reads
    // against the same host and the same per-IP allowance, and a second pool
    // would spend that allowance twice while each half counted only its own.
    net::RestClient rest{io, net::RestConfig{.host = std::string{rest_host()}}};

    std::unordered_map<std::uint64_t, DepthBook> books;

    // Channels the gateway asked for, owned as strings because the instrument
    // table's views outlive nothing in particular. A SET: Bbo and Depth name
    // one channel here, so subscribing both places one key.
    std::vector<std::string> desired;

    // Funding is not a channel. What the poll fans out to is this list, and an
    // empty one means nobody asked, which is why the poll never fires for a
    // gateway that only wants prints.
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // WHICH CONTRACTS AN ORDER MAY NAME, read once beside the catalogue from
    // /capi/v3/market/apiTradingSymbols. 290 of 1000 on 2026-09-15.
    //
    // The catalogue deliberately keeps the other 710: they stream a real book
    // and a scanner wants their prices. Only order entry consults this, and it
    // must — a create for a contract outside the set is answered -1058, which
    // reads in a log exactly like a bug in the adapter.
    //
    // EMPTY MEANS UNKNOWN, NOT EMPTY. If the call fails nothing is refused
    // locally, because refusing every order on a list that never arrived would
    // take the venue down over a missing read.
    std::unordered_set<std::string> api_tradable;

    [[nodiscard]] bool orderable(std::string_view symbol) const noexcept {
        return api_tradable.empty() || api_tradable.contains(std::string{symbol});
    }

    // Subscriptions the venue refused, and the last thing it said about one.
    // WEEX states no code on the socket, so the reason is prose.
    std::uint64_t subscribe_refusals{0};
    std::string last_error;

    // Adds a channel if it is new; returns true when it was.
    [[nodiscard]] bool add_channel(std::string_view channel) {
        for (const auto& existing : desired) {
            if (existing == channel) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(channel);
        return true;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// Makes the market-data REST clients ask the budget before every send.
//
// WITHOUT IT THE ALLOWANCE IS SPENT BY TRAFFIC NO COUNTER CAN SEE. WEEX meters
// WEIGHT PER IP, and its market data and its signed reads come out of the SAME
// pool — 500 weight in ten seconds, measured off x-remaining-weight-10s on
// 2026-09-07 — so one bucket covers both, and a second would spend the same
// allowance twice while each half counted only its own.
//
// A flat cost of 1 is the venue's own arithmetic for these two: the 685 KiB
// exchangeInfo and the 229 KiB premiumIndex each cost what a clock read does.
// The signed reads cost more and vary by endpoint, so they spend the same
// bucket explicitly rather than through a gate that knows one number.
//
// RateClass::Poll, never Order: nothing here may draw the bucket into a reserve
// an order path would need. A refusal is not an error — the funding poll asks
// again on its next tick.
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    auto* owner = &state;
    rest.set_admit([owner](std::string_view) {
        if (owner->budget == nullptr) {
            return true; // unmeasured venue: unlimited, not closed
        }
        return owner->budget
            ->take(exec::RateBucket::Shared, 1, owner->clock->mono(), exec::RateClass::Poll)
            .has_value();
    });
}

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Measures the offset to WEEX's clock, so a signature is stamped inside the
// 30-second window the venue enforces. Defined beside the signed client that
// shares its transport.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::weex::detail
