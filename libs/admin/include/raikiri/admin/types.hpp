#pragma once

// ids, nanosecond timestamps and prices are strings because they do not survive a double

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::admin {

struct GatewayHealth {
    bool alive{false};
    std::string generation;
    std::string last_heartbeat;
    // the catalogue size, not how much of it is subscribed
    std::int64_t instruments{0};
    // instruments whose quote cell has been written since the gateway started
    std::int64_t priced{0};
    // priced/instruments as a percentage, rounded down
    std::int64_t coverage_pct{0};
    // subscriptions the venue refused, summed across sockets
    std::int64_t subscribe_refusals{0};
};

struct Account {
    std::string account; // segment name; perp is unsuffixed
    std::string venue;
    std::string market;
    GatewayHealth gateway;
};

// signed durations stay integers; they fit a double's 53 bits for the next century
struct MeasuredClock {
    // "rest_probe" or "none"
    std::string method{};
    std::string endpoint{};
    std::optional<std::int64_t> offset_ns{};
    std::optional<std::int64_t> rtt_ns{};
    std::optional<std::int64_t> uncertainty_ns{};
    std::optional<std::string> measured_at{};
    std::optional<std::int64_t> measured_ago_ns{};
    // offset change since the measurement before, and how far apart the two were
    std::optional<std::int64_t> drift_ns{};
    std::optional<std::int64_t> drift_over_ns{};
    std::int64_t probe_failures{0};
    // "none", "rest", "feed", "rest_and_feed" or "conflict"
    std::string offset_source{};
    std::optional<std::int64_t> feed_bound_ns{};
    // an absent edge is open; low above high on a conflict
    std::optional<std::int64_t> offset_low_ns{};
    std::optional<std::int64_t> offset_high_ns{};
    std::int64_t correction_ns{0};
    // "raw" or "corrected"
    std::string lag_shown{};
};

struct ChannelFeed {
    std::string channel{};
    std::int64_t messages{0};
    std::int64_t window_ns{0};
    std::int64_t msgs_per_s{0};
    std::int64_t lag_samples{0};
    std::optional<std::int64_t> lag_p50_ns{};
    std::optional<std::int64_t> lag_p99_ns{};
    std::optional<std::int64_t> lag_p50_corrected_ns{};
    std::optional<std::int64_t> lag_p99_corrected_ns{};
    std::optional<std::int64_t> lag_min_ns{};
    std::optional<std::int64_t> last_message_age_ns{};
};

struct VenueMetrics {
    std::string account{};
    std::string venue{};
    std::string market{};
    bool alive{false};
    std::optional<std::string> published_at{};
    MeasuredClock clock{};
    std::int64_t catalogue{0};
    std::int64_t priced{0};
    std::int64_t reopens{0};
    std::vector<ChannelFeed> channels{};
};

struct DatabaseHealth {
    bool reachable{false};
    std::optional<double> lag_seconds;
};

// an upstream admind forwards to; any http answer counts as answering
struct UpstreamHealth {
    std::string name{};
    bool answering{false};
};

// `upstreams` is filled on GET /v1/health only, never on the stream's health feed
struct Health {
    std::vector<Account> gateways;
    DatabaseHealth database;
    std::optional<std::vector<UpstreamHealth>> upstreams{};
};

struct Instrument {
    std::string instrument_id;
    std::string account;
    std::string venue;
    std::string market;
    std::string exchange_symbol;
    std::string local_symbol;
    std::string tick_size;
    std::string lot_size;
    std::string min_notional;
    std::string contract_value;
    std::string contract_kind;
    std::string asset_class;
    // what cross-venue matching keys on; absent when the class is unknown
    std::optional<std::string> asset_key;

    // dated futures and options only
    std::optional<std::string> expiry;
    std::optional<std::string> strike;
    std::optional<std::string> put_call;
};

struct InstrumentPage {
    std::vector<Instrument> items;
    std::optional<std::string> next_cursor;
};

struct Listing {
    std::string instrument_id;
    std::string account;
    std::string venue;
    std::string market;
    std::string exchange_symbol;
    std::string local_symbol; // keeps a hip-3 deployer's prefix: "xyz:TSLA/USDC"
};

// one asset across every venue listing it, grouped server-side by its market key
struct Market {
    std::string key;    // "equity:TSLA/USD*"; an unclassified listing keys on its local symbol
    std::string symbol; // the local symbol most of its listings use
    std::vector<Listing> listings;
};

struct Quote {
    std::string instrument_id;
    std::string bid;
    std::string bid_size;
    std::string ask;
    std::string ask_size;
    std::string ts_event;
};

struct BookLevel {
    std::string price;
    std::string size;
};

struct Book {
    std::string instrument_id;
    std::vector<BookLevel> bids; // best first
    std::vector<BookLevel> asks; // best first
    std::string ts_event;
};

struct Greeks {
    std::string instrument_id;
    std::string delta;
    std::string gamma;
    std::string vega;
    std::string theta;
    std::string rho;
    std::string mark_iv; // a ratio: 0.2524 is 25.24 %
    std::string bid_iv;
    std::string ask_iv;
    std::string underlying_price; // quote currency, not the option's ticks
    std::string mark_price;       // the premium's own currency
    std::string index_price;      // quote currency
    std::string ts_event;
};

struct Funding {
    std::string instrument_id;
    std::string rate; // per interval, not an APR
    std::string predicted_rate;
    std::string mark_price;
    std::optional<std::string> index_price; // absent where the venue publishes none
    std::string next_funding_time;
    std::optional<std::int64_t> interval_seconds;
    std::optional<std::string> accrual; // discrete | continuous; absent when unknown
    std::string ts_event;
};

// the venue's rolling 24 h turnover in base and quote units
struct Ticker {
    std::string instrument_id;
    std::string volume_24h_base;
    std::string volume_24h_quote;
    std::optional<std::string> open_interest; // base units; absent where the venue does not publish it
    std::string ts_event;
};

struct Position {
    std::string instrument_id;
    std::int64_t strategy_id{0};
    std::string qty; // signed; positive is long
    std::string avg_entry_price;
    std::string realized;
    // null is not flat: the venue could not report or the pull failed
    std::optional<std::string> venue_qty;
    std::optional<std::string> venue_reported_at;
    std::string ts_updated;
};

// a currency balance as the venue last reported it; the set travels together
struct Balance {
    std::string asset;
    // absent when the venue did not report it
    std::optional<std::string> total;
    std::optional<std::string> available;
    std::optional<std::string> unrealized;
};

struct AccountBalances {
    std::string account;
    std::string venue;
    // absent until the venue answers
    std::optional<std::string> reported_at;
    bool stale{false};
    std::vector<Balance> assets;
};

// rfc 9457
struct Problem {
    std::string title;
    std::int32_t status{0};
    // braced so a problem with only title and status stays legal
    std::optional<std::string> detail{};
};

} // namespace raikiri::admin
