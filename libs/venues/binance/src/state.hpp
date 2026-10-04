#pragma once

// anonymous types in a header differ per includer

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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
#include <unordered_set>
#include <vector>

namespace raikiri::venues::binance::internal {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag, MarketType market_type)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag}, market{market_type} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // spot is market data only, on its own hosts and weight pool
    MarketType market;
    // binance counts one weight pool per ip, shared with the account poller
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder{signer, market};

    // its own pool so seeds and orders do not queue behind each other; six connections for catalogue-wide seeding
    net::RestClient depth_rest{io, net::RestConfig{.host = std::string{rest_host(testnet, market)}, .pool_size = 6}};
    std::unordered_map<std::uint64_t, DepthBook> books;

    std::vector<std::string> desired_streams;

    // funding is polled: @markPrice is acknowledged but sends nothing here
    std::vector<InstrumentId> funding_instruments;
    std::uint64_t next_funding_poll_ns{0};

    // the 24h table reaches these: funding subscriptions on perp, every subscription on spot
    std::vector<InstrumentId> ticker_instruments;

    // seeds are rest calls the socket pool does not pace; the queue paces them and meter_market_data charges them
    std::vector<InstrumentId> pending_seeds;
    std::uint64_t next_seed_ns{0};

    // venue clock offset, measured against the market's time endpoint
    std::int64_t clock_offset_ns{0};

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    std::string listen_key;
    std::uint64_t listen_key_due_ns{0};
    bool listen_key_expired{false};

    // each dropped private frame is an order update the book never sees
    std::uint64_t private_frames_dropped{0};

    std::function<void(UnixNanos)> tick_hook;

    // an option book ticker says nothing until the top moves, so the mark row quotes it until then
    std::unordered_set<std::uint64_t> option_quote_due;

    // per SymbolId: 1 quoted two-sided, 2 greeks, 4 traded; logged once a minute on an option gateway
    std::vector<std::uint8_t> option_coverage;
    std::uint64_t next_coverage_ns{0};

    void cover(InstrumentId id, std::uint8_t bit) {
        if (id.market() != MarketType::Option) {
            return;
        }
        const auto index = id.symbol().raw;
        if (option_coverage.size() <= index) {
            option_coverage.resize(index + 1);
        }
        option_coverage[index] |= bit;
    }

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    [[nodiscard]] bool spot() const noexcept { return market == MarketType::Spot; }

    // spot and options stream market data and take no orders
    [[nodiscard]] bool market_data_only() const noexcept { return market != MarketType::Perp; }

    void add_stream(std::string_view stream) {
        for (const auto& existing : desired_streams) {
            if (existing == stream) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_streams.emplace_back(stream);
    }
};

// market-data rest spends the venue's limits as a poll; costs come from config
inline void meter_market_data(VenueState& state, net::RestClient& rest) {
    rest.set_admit(exec::market_data_gate(state.budget, *state.clock));
}

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

} // namespace raikiri::venues::binance::internal
