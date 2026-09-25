#pragma once

// shared by the data and execution clients: one host, one key, one instrument table

#include <raikiri/common/time.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/venues/bybit/ws.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::bybit::internal {

// bybit's idle timeout is 20 s and it drops connections that answer only control frames
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// single-threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // shared with the account poller; null means unlimited
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};
    // one of k_book_tiers; bybit names the tier in the topic
    std::uint32_t book_depth{k_book_depth};

    // what each instrument's ticker frames have said so far
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;

    net::IoContext io;
    net::Signer signer;
    exec::InstrumentTable instruments;
    Protocol protocol;

    std::vector<std::string> desired_topics;

    std::unordered_map<InstrumentId, BookSync> books;

    std::uint64_t next_app_ping_ns{0};

    std::int64_t clock_offset_ns{0};

    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_topic(std::string_view topic) {
        for (const auto& existing : desired_topics) {
            if (existing == topic) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired_topics.emplace_back(topic);
    }

    BookSync& book_for(const Instrument& instrument) {
        const auto& base = base_of(instrument);
        const auto it = books.find(base.id);
        if (it != books.end()) {
            return it->second;
        }
        return books.emplace(base.id, BookSync{base.id, base.tick_size, base.lot_size}).first->second;
    }
};

inline constexpr std::size_t k_max_unanswered = 512;

// cold path: pumps the reactor until the answer arrives
inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// defined beside the catalogue loader
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::bybit::internal
