#pragma once

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::config {

// `kind` is rest, ws_messages, ws_connects, ws_streams, ws_sockets, ws_channels or ws_frame
struct RateLimitSettings {
    std::string name{};
    std::string kind{"rest"};
    std::string scope{};
    std::string market{}; // "perp" or "spot"; empty applies to both
    std::uint64_t window_ms{0};
    std::uint32_t max{0};
    std::uint32_t reserve{0};
    std::vector<std::string> match{};
};

struct RateCostSettings {
    std::string match;
    std::uint32_t cost{1};
};

struct VenueSettings {
    // zero disables the timeout
    std::uint64_t order_answer_timeout_ms{15'000};

    // zero keeps the adapter's tier and an unknown tier is refused; on bybit a deeper tier is also
    // a slower cadence, bbo included
    std::uint32_t book_depth{0};

    // empty keeps the adapter's feed; kucoin "uta" takes the unified account's obu channel, which
    // needs no rest seed
    std::string market_data_feed;

    std::vector<RateLimitSettings> limits;
    std::vector<RateCostSettings> costs;
    std::uint32_t default_cost{1};
};

struct VenuesFile {
    std::map<std::string, VenueSettings> venues;
};

struct DatabaseSettings {
    // a reference such as env:NAME, since a dsn carries a password
    std::string dsn;
};

struct DatabaseFile {
    DatabaseSettings database;
};

// a missing file is an error here; the caller decides whether it is required
[[nodiscard]] std::expected<VenuesFile, std::string> load_venues(std::string_view path);
[[nodiscard]] std::expected<DatabaseFile, std::string> load_database(std::string_view path);

// The venue's first limit of `kind` (and `scope`, when given), or none.
[[nodiscard]] const RateLimitSettings*
find_limit(const VenueSettings& venue, std::string_view kind, std::string_view scope = {}) noexcept;

// Where the files live when nothing on the command line says otherwise:
// $RAIKIRI_CONFIG_DIR, else "config" beside the working directory.
[[nodiscard]] std::string config_dir();

} // namespace raikiri::config
