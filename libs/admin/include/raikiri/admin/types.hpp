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

struct DatabaseHealth {
    bool reachable{false};
    std::optional<double> lag_seconds;
};

struct Health {
    std::vector<Account> gateways;
    DatabaseHealth database;
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
};

// one market across every venue listing it, grouped server-side
struct Market {
    std::string symbol; // canonical: "BTC/USDT"
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
    std::string ts_event;
};

struct Funding {
    std::string instrument_id;
    std::string rate; // per interval, not an APR
    std::string predicted_rate;
    std::string mark_price;
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
