#pragma once

// What the three coinbase clients share, and nothing else.
//
// The public socket, the private socket and the catalogue are separate
// concerns that happen to run against one connection state. Only that state
// crosses the boundary; each client keeps its own constants and its own
// helpers in its own translation unit, where a reader looking for one of them
// is not made to scroll past the other two.
//
// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/caps.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/exec/order_gate.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_client.hpp>
#include <raikiri/venues/coinbase/book.hpp>
#include <raikiri/venues/coinbase/errors.hpp>
#include <raikiri/venues/coinbase/map.hpp>
#include <raikiri/venues/coinbase/rest.hpp>
#include <raikiri/venues/coinbase/ws.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raikiri::venues::coinbase::detail {

// Far above any sane working set; the cap exists so a venue incident cannot
// turn the client-id memo into the heap.
constexpr std::size_t k_max_tracked_orders = 65'536;

// A subscription the gateway asked for, owned as strings because the
// instrument table's views outlive nothing in particular.
struct OwnedSubscription {
    std::string channel;
    std::string product_id;
};

// State every coinbase connection shares within one gateway process. Single
// threaded by contract: the gateway loop is the only caller, which is why
// nothing here synchronises.
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref)
        : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    net::Signer signer;
    // Coinbase's third credential. Held as a string because it travels in a
    // header on every signed request and is not itself a signing key.
    std::string passphrase;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_confirmed{0};
    std::uint64_t subscribe_refusals{0};
    exec::VenueError last_error{};

    // The UUID this adapter derived from a ULID cannot be turned back into
    // one, so the mapping is remembered for as long as the process lives. An
    // order placed by an EARLIER process comes back from a sweep with no
    // client id, which is a gap reconciliation must be told about rather than
    // one that is quietly filled with a wrong id.
    std::unordered_map<std::string, ExchangeClientOrderId> client_ids;

    // Deribit-style venue clock correction: Coinbase rejects a signature more
    // than 30 seconds from its own clock.
    [[nodiscard]] UnixNanos venue_now() const noexcept {
        const auto local = static_cast<std::int64_t>(clock->wall().ns);
        return UnixNanos{static_cast<std::uint64_t>(local + clock_offset_ns)};
    }

    void remember(const ExchangeClientOrderId& id) {
        const auto uuid = client_oid_of(id);
        if (!uuid.has_value() || client_ids.size() >= k_max_tracked_orders) {
            return;
        }
        client_ids.emplace(std::string{uuid->data(), uuid->size()}, id);
    }

    [[nodiscard]] ExchangeClientOrderId recall(std::string_view client_oid) const {
        const auto at = client_ids.find(std::string{client_oid});
        return at == client_ids.end() ? ExchangeClientOrderId{} : at->second;
    }

    void add_subscription(std::string_view channel, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.product_id == product_id) {
                return; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{product_id});
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

// Measured once at startup: a signature stamped from a clock the venue does
// not share reads to it as a bad signature, not as a late one.
void sync_clock(VenueState& state, net::RestClient& rest);

} // namespace raikiri::venues::coinbase::detail
