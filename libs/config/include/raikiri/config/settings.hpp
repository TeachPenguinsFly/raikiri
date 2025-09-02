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
