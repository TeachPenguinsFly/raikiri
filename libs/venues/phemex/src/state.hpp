#pragma once

// `detail`, not an anonymous namespace: a type declared in a header inside an
// anonymous namespace is a DIFFERENT type in every file that includes it.

#include <raikiri/exec/events.hpp>
#include <raikiri/exec/rate_budget.hpp>
#include <raikiri/exec/instrument_table.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/venues/phemex/map.hpp>
#include <raikiri/venues/phemex/market_data.hpp>
#include <raikiri/venues/phemex/ws.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace raikiri::venues::phemex::detail {

struct VenueState {
    VenueState(exec::GatewaySink& sink_ref, const Clock& clock_ref, bool testnet_flag)
        : sink{&sink_ref}, clock{&clock_ref}, testnet{testnet_flag} {}

    exec::GatewaySink* sink;
    const Clock* clock;
    exec::RateBudget* budget{nullptr};
    bool testnet;

    net::IoContext io;
    exec::InstrumentTable instruments;
    Protocol protocol;
    MarketType market{MarketType::Perp};

    std::unordered_map<std::uint64_t, Listing> listings; // InstrumentId::packed
    std::unordered_map<std::uint64_t, Book> books;

    // Bbo and Depth are one book key; every perp's funding is one pack key.
    std::unordered_set<std::string> channels;
    std::unordered_set<std::uint64_t> funding_wanted;
    // The book is also the quote, so one asked for BBO alone must not publish its ladder.
    std::unordered_set<std::uint64_t> depth_wanted;

    std::unordered_set<std::uint64_t> quoted;
    std::unordered_set<std::uint64_t> traded;
    std::unordered_set<std::uint64_t> funded;
    // Live on an all-symbol feed but absent from the (CDN-stale) catalogue.
    std::unordered_set<std::string> unlisted;

    PackColumns pack_columns;

    std::uint64_t subscribe_refusals{0};
    std::int64_t last_error_code{0};
    std::uint64_t off_grid_frames{0};

    struct Found {
        const InstrumentBase* base;
        Listing* listing;
    };

    [[nodiscard]] std::optional<Found> find(std::string_view symbol) {
        const auto* instrument = instruments.find(symbol);
        if (instrument == nullptr) {
            return std::nullopt;
        }
        const auto& base = base_of(*instrument);
        const auto listing = listings.find(base.id.packed);
        if (listing == listings.end()) {
            return std::nullopt;
        }
        return Found{&base, &listing->second};
    }
};

} // namespace raikiri::venues::phemex::detail
