#pragma once

// What every bybit connection in one process shares.
//
// INTERNAL TO THE ADAPTER — it is under src/ rather than include/ because
// nothing outside these translation units may depend on it. The public surface
// of a venue is the three exec:: interfaces and nothing else.
//
// The data client and the execution client hold the SAME state, because they
// speak to one host with one key: two copies would mean two connections, two
// signatures and two instrument tables that drift apart.

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

namespace raikiri::venues::bybit::detail {

// Bybit's own ping cadence. Its documented idle timeout is 20 s and it drops
// connections that answer only the websocket control frame, so the interval
// is comfortably inside that rather than at it.
constexpr std::uint64_t k_app_ping_interval_ns = 15'000'000'000ULL;

// State every bybit connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    bool testnet;
    // Shared with the account poller, because bybit counts both against one IP.
    // Null means unmeasured, which reads as unlimited rather than as closed.
    exec::RateBudget* budget{nullptr};
    std::uint64_t order_answer_timeout_ns{0};
    // One of k_book_tiers. Set once at construction and read on every
    // subscribe, because bybit names the tier inside the topic string.
    std::uint32_t book_depth{k_book_depth};

    // What each instrument's ticker frames have said so far. A frame is a
    // partial and `FundingUpdate` is written whole — see market_data.hpp.
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
        return UnixNanos{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
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
        return books.emplace(base.id, BookSync{base.id, base.tick_size, base.lot_size})
            .first->second;
    }
};

// A desk with more than this many orders awaiting an answer at once has a
// problem no table size fixes.
inline constexpr std::size_t k_max_unanswered = 512;

// Cold-path request/response. Every caller of this is a startup or
// reconciliation path, never the hot loop, so pumping the reactor until the
// answer arrives is exactly right: the alternative — an asynchronous
// catalogue load — would force every venue to invent its own half-started
// state, and a gateway that trades before it knows its tick sizes is worse
// than one that takes an extra second to boot.
inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// Defined beside the catalogue loader it shares a transport with.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::bybit::detail
