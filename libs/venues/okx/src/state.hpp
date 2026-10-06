#pragma once

// internal namespace: an anonymous one in a header is a distinct type per includer

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

namespace raikiri::venues::okx::internal {

struct OwnedSubscription {
    std::string channel;
    std::string inst_id;
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

    // okx splits rate and mark across two channels; the record is written whole
    std::unordered_map<std::uint32_t, FundingMemo> funding_memo;
    // from the open-interest poll, stamped into the tickers record
    std::unordered_map<InstrumentId, FixedE9> open_interest;

    net::IoContext io;
    net::Signer signer;
    config::Secret passphrase;
    exec::InstrumentTable instruments;
    // the order socket names an instrument by this code alone
    std::unordered_map<InstrumentId, std::uint64_t> inst_id_codes;
    Protocol protocol;
    // its own instance so the private socket's scratch stays separate
    Protocol private_protocol;
    RequestBuilder builder;

    // the private order channel subscribes by instrument type
    bool spot_enabled{false};
    bool perp_enabled{true};
    bool option_enabled{false};
    // comma-separated option families to load, e.g. BTC-USD,ETH-USD_UM; empty loads every one
    std::string families;

    // by SymbolId; the index is shared by an underlying's families
    std::unordered_map<std::uint32_t, OptionMemo> option_memo;
    std::unordered_map<std::string, FixedE9> index_prices;

    // per SymbolId: 1 quoted two-sided, 2 greeks, 4 traded; logged once a minute on an option gateway
    std::vector<std::uint8_t> option_coverage;
    std::uint64_t next_coverage_ns{0};

    void cover(InstrumentId id, std::uint8_t bit) {
        if (!option_enabled || id.market() != MarketType::Option) {
            return;
        }
        const auto index = id.symbol().raw;
        if (option_coverage.size() <= index) {
            option_coverage.resize(index + 1);
        }
        option_coverage[index] |= bit;
    }

    std::vector<OwnedSubscription> desired;

    std::unordered_map<InstrumentId, Book> books;
    std::uint64_t next_app_ping_ns{0};
    std::uint64_t next_resync_scan_ns{0};

    // Venue clock offset, measured against /api/v5/public/time.
    std::int64_t clock_offset_ns{0};

    // Subscriptions the venue refused.
    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};

    [[nodiscard]] bool credentialed() const noexcept { return signer.has_credentials() && !passphrase.empty(); }

    // okx refuses a signature more than 30 s off its clock (50102)
    [[nodiscard]] UnixNanos venue_now() const {
        const auto local = clock->wall();
        return UnixNanos{static_cast<std::uint64_t>(static_cast<std::int64_t>(local.ns) + clock_offset_ns)};
    }

    void add_subscription(std::string_view channel, std::string_view inst_id) {
        for (const auto& existing : desired) {
            if (existing.channel == channel && existing.inst_id == inst_id) {
                return; // refcounting lives in the gateway core
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

} // namespace raikiri::venues::okx::internal
