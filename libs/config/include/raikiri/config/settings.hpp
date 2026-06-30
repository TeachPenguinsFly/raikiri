#pragma once

// Everything an operator tunes, read from files rather than compiled in.
//
// NO OPERATIONAL NUMBER BELONGS IN THE BINARY. A venue's budget changes when
// the venue changes it, not when we ship; a database moves without the trading
// code being touched. Both were hardcoded once and both were wrong within a
// day of the venue's own headers being read.
//
// Everything here is COLD PATH: read once at startup, never on the order path.

#include <raikiri/domain/identifiers.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::config {

// One limit a venue states. `kind` says what it counts:
//   rest         REST cost in a sliding window, over the paths in `match`
//   ws_messages  outbound socket frames in a window, per `scope` socket or ip
//   ws_connects  connection attempts in a window, per ip
//   ws_streams   subscriptions one socket may carry
//   ws_sockets   sockets one ip may hold
//   ws_channels  subscriptions across every socket of the ip
//   ws_frame     subscriptions one subscribe frame may carry
struct RateLimitSettings {
    std::string name;
    std::string kind{"rest"};
    std::string scope;
    std::string market; // "perp" or "spot"; empty applies to both
    std::uint64_t window_ms{0};
    std::uint32_t max{0};
    std::uint32_t reserve{0};
    std::vector<std::string> match;
};

struct RateCostSettings {
    std::string match;
    std::uint32_t cost{1};
};

struct VenueSettings {
    // Zero means "never expire", which is the wrong default for money: a
    // gateway that never notices an unanswered order holds it silently.
    std::uint64_t order_answer_timeout_ms{15'000};

    // Which of the venue's book tiers to subscribe. Zero keeps the adapter's
    // own choice, and an unknown tier is refused rather than approximated —
    // venues publish a FIXED set and answer anything else with an error that
    // costs the whole subscription.
    //
    // A TIER IS A CADENCE AS WELL AS A DEPTH, and the two move in opposite
    // directions. Measured against bybit's whole linear catalogue on
    // 2026-09-18, 811 symbols over two minutes: orderbook.50 pushes at 20 ms
    // for 4394 messages a second, orderbook.200 at 100 ms for 2244 — half the
    // messages and 65% of the bytes, still carrying 90% of the level updates,
    // because five 20 ms deltas on one symbol overlap and collapse into one.
    //
    // So it is a deployment decision and not a default. A recorder sampling
    // on a one-second grid should take the deeper, slower tier; anything
    // quoting off the top of the book should not, because the same setting
    // slows ITS bbo to 100 ms — bybit serves both from one topic.
    std::uint32_t book_depth{0};

    // Which of a venue's market-data feeds to take, where it publishes more
    // than one. Empty keeps the adapter's own choice, which is what every
    // venue here did before any of them had a second.
    //
    // kucoin: "uta" takes the unified account's `obu` channel instead of the
    // classic /contractMarket/level2. Measured on all 683 contracts,
    // 2026-09-19: 4.76x fewer messages and 4.18x fewer bytes for the same
    // symbols, because the venue conflates at 10 ms rather than publishing
    // every level that moves. It also needs no REST seed at all, which
    // removes the class of bug that had 677 books asking for a snapshot
    // 27,391 times in seven minutes.
    std::string market_data_feed;

    std::vector<RateLimitSettings> limits;
    std::vector<RateCostSettings> costs;
    std::uint32_t default_cost{1};
};

struct VenuesFile {
    std::map<std::string, VenueSettings> venues;
};

struct DatabaseSettings {
    // A REFERENCE, not a value: "env:NAME", "file:/path", "exec:cmd". A DSN
    // carries a password, and this file is committed.
    std::string dsn;
};

struct DatabaseFile {
    DatabaseSettings database;
};

// Reads the file, or says why it could not. A MISSING FILE IS AN ERROR HERE
// and a tolerable condition for the caller: only the caller knows whether the
// thing it configures is required. Neither of these decides that.
[[nodiscard]] std::expected<VenuesFile, std::string> load_venues(std::string_view path);
[[nodiscard]] std::expected<DatabaseFile, std::string> load_database(std::string_view path);

// The venue's first limit of `kind` (and `scope`, when given), or none.
[[nodiscard]] const RateLimitSettings*
find_limit(const VenueSettings& venue, std::string_view kind, std::string_view scope = {}) noexcept;

// Where the files live when nothing on the command line says otherwise:
// $RAIKIRI_CONFIG_DIR, else "config" beside the working directory.
[[nodiscard]] std::string config_dir();

} // namespace raikiri::config
