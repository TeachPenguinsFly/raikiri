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

namespace raikiri::config {

// One meter a venue keeps. `capacity` of 0 disables it — a budget nobody has
// established must not be invented, because refusing traffic the venue would
// have accepted is the worse of the two errors.
struct RateBucketSettings {
    std::uint32_t capacity{0};
    std::uint64_t window_ms{0};
    // What only order traffic may spend, so a reconciliation sweep can never
    // consume the allowance a cancel is about to need.
    std::uint32_t reserve{0};
};

// What one call costs in the venue's own units: binance charges weight per
// endpoint, bybit counts requests.
struct RateCostSettings {
    std::uint32_t order{1};
    std::uint32_t positions{1};
    std::uint32_t balances{1};
    std::uint32_t ledger{1};
};

struct VenueRateSettings {
    RateBucketSettings shared{};
    RateBucketSettings order_entry{};
    RateBucketSettings account{};
    RateBucketSettings ledger{};
    RateCostSettings costs{};
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

    VenueRateSettings rate{};
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

// Where the files live when nothing on the command line says otherwise:
// $RAIKIRI_CONFIG_DIR, else "config" beside the working directory.
[[nodiscard]] std::string config_dir();

} // namespace raikiri::config
