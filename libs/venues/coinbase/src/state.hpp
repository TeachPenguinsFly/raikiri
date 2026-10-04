#pragma once

// internal namespace: an anonymous namespace in a header is a different type per file

#include <raikiri/config/secret.hpp>
#include <raikiri/exec/events.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>
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

namespace raikiri::venues::coinbase::internal {

// caps the client-id memo
constexpr std::size_t k_max_tracked_orders = 65'536;

struct OwnedSubscription {
    std::string channel;
    std::string product_id;
};

// single-threaded: the gateway loop is the only caller
struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref) : sink{&sink_ref}, clock{&clock_ref} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    std::uint64_t order_answer_timeout_ns{0};

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    RequestBuilder builder;
    net::Signer signer;
    // the third credential, sent as a header on every signed request
    std::string passphrase;

    std::vector<OwnedSubscription> desired;
    std::unordered_map<std::uint64_t, Book> books;

    std::int64_t clock_offset_ns{0};

    std::uint64_t channels_confirmed{0};
    std::uint64_t subscribe_refusals{0};
    exec::VenueError last_error{};

    // uuid to ulid memo for this process; orders from an earlier process come back without a client id
    std::unordered_map<std::string, ExchangeClientOrderId> client_ids;

    // coinbase rejects signatures more than 30 s off its clock
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

    // false when already asked for; the pool counts keys against socket room
    [[nodiscard]] bool add_subscription(std::string_view channel, std::string_view product_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.product_id == product_id) {
                return false; // refcounting lives in the gateway core; this is a set
            }
        }
        desired.emplace_back(std::string{channel}, std::string{product_id});
        return true;
    }

    Book& book_for(const InstrumentBase& base) {
        return books.try_emplace(base.id.packed, base.id, base.tick_size, base.lot_size).first->second;
    }
};

inline std::optional<net::RestResponse> await_response(VenueState& state,
                                                       net::RestClient& rest,
                                                       const net::RestRequest& request,
                                                       std::uint64_t timeout_ms = 10'000) {
    return net::await_response(state.io, *state.clock, rest, request, timeout_ms);
}

// measured once at startup

} // namespace raikiri::venues::coinbase::internal
