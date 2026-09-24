#pragma once

// internal namespace: an anonymous one in a header is a distinct type per translation unit

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

// fallback when the bullet omits its ping interval
constexpr std::uint64_t k_default_ping_ms = 18'000;

constexpr std::uint64_t k_reopen_delay_ns = 1'000'000'000ULL;

// how long a runtime reopen waits for its bullet token
constexpr std::uint64_t k_bullet_deadline_ns = 10'000'000'000ULL;

struct OwnedSubscription {
    std::string topic;
    std::string symbol;
};

// a signed rest call mints the one-shot token and ping cadence the private socket opens with
struct PrivateTicket {
    StreamEndpoint endpoint;
    std::uint64_t ping_interval_ms{0};
};

// timers driven from the data client's poll; the private socket needs an application ping
class Pumped {
public:
    Pumped() = default;
    Pumped(const Pumped&) = delete;
    Pumped& operator=(const Pumped&) = delete;

    virtual void pump(UnixNanos now) noexcept = 0;

protected:
    ~Pumped() = default;
};

// shared per gateway process; single threaded, nothing locks
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    // shared with the order path since kucoin meters one allowance per ip; null is unlimited
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    // the trees share subject names for different payloads, so the market is fixed per process
    MarketType market{MarketType::Perp};

    // false is the classic feed, true is uta v2; chosen in config/venues.yaml
    bool uta{false};

    std::unordered_map<std::uint64_t, UtaBook> uta_books;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    Credentials credentials;

    std::vector<OwnedSubscription> desired;
    std::vector<Pumped*> pumps;

    // funding rate and mark arrive on separate subjects; see market_data.hpp
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    // own pool so a resync never queues behind the bullet or catalogue
    net::RestClient depth_rest{io,
                               net::RestConfig{.host = std::string{rest_host()}, .port = "443"}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::uint64_t app_ping_interval_ns{k_default_ping_ms * 1'000'000ULL};
    std::uint64_t next_app_ping_ns{0};

    bool reopen_due{false};
    std::uint64_t reopen_at_ns{0};
    std::uint64_t connect_seq{0};

    // offset to /api/v1/timestamp; signatures are refused 5 s off
    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    // checked before every market-data rest send; poll class, so seeds never spend the order reserve
    [[nodiscard]] bool admit_poll(std::string_view target = {}) noexcept {
        if (budget == nullptr) {
            return true; // unmeasured venue: unlimited, not closed
        }
        return budget->take(target, clock->mono(), exec::RateClass::Poll)
            .has_value();
    }

    // a 429 holds spending until gw-ratelimit-reset (ms)
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

    // null for a symbol this gateway did not list
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
