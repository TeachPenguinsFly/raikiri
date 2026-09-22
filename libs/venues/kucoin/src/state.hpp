#pragma once

// `internal`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/kucoin/book.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/venues/kucoin/errors.hpp>
#include <raikiri/venues/kucoin/map.hpp>
#include <raikiri/venues/kucoin/rest.hpp>
#include <raikiri/venues/kucoin/ws.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::kucoin::internal {

// The bullet response states 18 s; this is the fallback if it ever omits it.
constexpr std::uint64_t k_default_ping_ms = 18'000;

// Long enough that a venue outage does not turn into a bullet-public flood,
// short enough that a token rejection costs one second of data.
constexpr std::uint64_t k_reopen_delay_ns = 1'000'000'000ULL;

// How long a runtime reopen waits for its bullet token before asking again.
// Ten seconds is what the blocking version allowed, and it is kept — the
// difference is that the gateway loop now runs throughout instead of standing
// still for it.
constexpr std::uint64_t k_bullet_deadline_ns = 10'000'000'000ULL;

struct OwnedSubscription {
    std::string topic;
    std::string symbol;
};

// What a private socket needs to open. KuCoin puts a one-shot token in the
// stream URL and states the ping cadence beside it, and only a signed REST
// call mints one — which is why the socket cannot open before the REST pool
// has, and why this vocabulary is shared by both halves rather than owned by
// either.
struct PrivateTicket {
    StreamEndpoint endpoint;
    std::uint64_t ping_interval_ms{0};
};

// Anything with a timer of its own registers here and is driven from the DATA
// client's poll. The private socket needs one: KuCoin closes a connection
// whose application-level ping stops arriving, and the websocket control frame
// does not count as one.
//
// gatewayd also polls the execution client, so this registry is not the only
// way to get a tick — it is the way that keeps every timer in this adapter on
// one clock.
class Pumped {
public:
    Pumped() = default;
    Pumped(const Pumped&) = delete;
    Pumped& operator=(const Pumped&) = delete;

    virtual void pump(UnixNanos now) noexcept = 0;

protected:
    ~Pumped() = default;
};

// State every kucoin connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, so nothing locks.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    // Shared with the order path: KuCoin meters one allowance per IP, so two
    // budgets would each believe they had the whole of it. Null means the
    // venue is unmeasured, which reads as unlimited rather than as closed.
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // The one market this process serves. KuCoin's two trees are separate
    // deployments reached through separate bullets, and they share a subject
    // name ("level2") for two different payloads, so nothing below can read a
    // frame without knowing which socket it arrived on.
    MarketType market{MarketType::Perp};

    // TWO FEEDS, ONE ADAPTER. False is the classic tree — a bullet minted over
    // REST, colon-joined topics, one message per price level that moves and a
    // book that must be seeded from /api/v1/level2/snapshot. True is UTA v2:
    // no token, one subscribe frame per symbol, a whole book followed by
    // deltas conflated at 10 ms, and no seed to fetch.
    //
    // Chosen per deployment in config/venues.yaml, not here, because which one
    // a desk wants depends on what reads the feed — and because the classic
    // path must stay reachable until this one has run a week.
    bool uta{false};

    std::unordered_map<std::uint64_t, UtaBook> uta_books;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    Credentials credentials;

    std::vector<OwnedSubscription> desired;
    std::vector<Pumped*> pumps;

    // What each contract's two funding subjects have said so far. KuCoin
    // splits the rate and the mark across them and the record is written
    // whole — see market_data.hpp.
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    // The seed rides its own pool so a book resynchronising never queues
    // behind the bullet token or the catalogue.
    net::RestClient depth_rest{io,
                               net::RestConfig{.host = std::string{rest_host()}, .port = "443"}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::uint64_t app_ping_interval_ns{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_app_ping_ns{0};

    bool reopen_due{false};
    std::uint64_t reopen_at_ns{0};
    std::uint64_t connect_seq{0};

    // Measured against /api/v1/timestamp. KuCoin refuses signatures more than
    // five seconds from ITS clock, so a laptop drifting looks exactly like a
    // bad key until the offset is applied.
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    // ASKED BEFORE EVERY MARKET-DATA REST SEND, and a false refuses it. KuCoin
    // counts weight, priced per path in config/venues.yaml, so the target must
    // reach the budget. Poll, never Order: a seed may not spend a cancel's reserve.
    [[nodiscard]] bool admit_poll(std::string_view target = {}) noexcept {
        if (budget == nullptr) {
            return true; // unmeasured venue: unlimited, not closed
        }
        return budget->take(target, clock->mono(), exec::RateClass::Poll)
            .has_value();
    }

    // A 429 spends nothing more until the pool refills, which KuCoin states in
    // gw-ratelimit-reset (ms).
    void hold_after_refusal(const net::RestResponse& response) noexcept {
        if (budget == nullptr || response.status != 429) {
            return;
        }
        budget->penalize_all(clock->mono(), refusal_hold_ms(response.rate_headers) * 1'000'000ULL);
    }

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    void add_subscription(std::string_view topic, std::string_view symbol) {
        for (const auto& existing : desired) {
            if (existing.topic == topic && existing.symbol == symbol) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{topic}, std::string{symbol});
    }

    void schedule_reopen() noexcept {
        reopen_due = true;
        reopen_at_ns = clock->mono().ns + k_reopen_delay_ns;
    }

    [[nodiscard]] std::string next_connect_id() {
        return "raikiri" + std::to_string(clock->wall().ns + connect_seq++);
    }

    // Null when the symbol is not one this gateway listed: there is nothing to
    // accumulate into and nothing the mapper could publish anyway.
    FundingMemo* memo_for(std::string_view symbol) {
        const auto* listed = instruments.find(symbol);
        if (listed == nullptr) {
            return nullptr;
        }
        return &funding_memo[base_of(*listed).id.symbol().raw];
    }

    UtaBook& uta_book_for(const InstrumentBase& base) {
        return uta_books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size)
            .first->second;
    }

    DepthBook& book_for(const InstrumentBase& base) {
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

// Turns a REST answer into a usable body, or into the classified failure the
// gateway acts on.
inline std::expected<std::string, exec::VenueError>
usable_body(const std::optional<net::RestResponse>& response) {
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
        return std::unexpected{
            exec::VenueError::make(classify(envelope->code), envelope->code, envelope->msg)};
    }
    return response->body;
}

} // namespace raikiri::venues::kucoin::internal
