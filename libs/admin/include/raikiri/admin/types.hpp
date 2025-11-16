#pragma once

// The response shapes of schema/admin-api.yaml, as C++.
//
// EVERY NUMBER THAT CANNOT SURVIVE A DOUBLE IS A STRING here, exactly as it is
// there. JavaScript holds integers exactly only to 2^53 and a nanosecond
// timestamp is already 1.79e18, so a ns value sent as a JSON number reaches
// the browser rounded to the nearest ~200 ns; instrument and order ids are
// packed 64-bit words with the venue at bit 56 and overflow the same way; and
// a price in a double renders as 79418.299999996.
//
// So the conversions happen HERE, once, at the edge where the domain's
// fixed-point words become text — and every field below is either a string or
// a value small enough to be a number honestly.
//
// These structs are serialised by glaze, which is a cold-path dependency on
// purpose: nothing an order waits on is built from them.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raikiri::admin {

// ------------------------------------------------------------- system ----

struct GatewayHealth {
    bool alive{false};
    std::string generation;
    std::string last_heartbeat;
    // What the venue LISTS. It is the catalogue's size and nothing else — a
    // gateway carrying a third of it reports the same number as one carrying
    // all of it.
    std::int64_t instruments{0};
    // WHAT IS ACTUALLY ARRIVING, which is the question `alive` cannot answer.
    //
    // A gateway whose subscriptions the venue silently dropped is alive, is
    // heartbeating, and lists its whole catalogue — htx ran at 65 of 340 for
    // days and every field above said it was fine. This counts instruments
    // whose quote cell has been written recently, which is the same measure
    // tools/coverage_smoke.sh takes from outside, taken from inside and
    // continuously.
    std::int64_t priced{0};
    // priced/instruments as a percentage, rounded down, so a screen and an
    // alert agree on one number rather than each deriving its own.
    std::int64_t coverage_pct{0};
    // Subscriptions the venue refused outright, summed across the adapter's
    // sockets. Distinct from the silent kind above: a refusal was answered,
    // and until now the count reached nothing that could report it.
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

// ---------------------------------------------------------- catalogue ----

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

    // Dated futures and options only; absent elsewhere rather than zero,
    // because an expiry of zero is a date in 1970 and a strike of zero is a
    // free option.
    std::optional<std::string> expiry;
    std::optional<std::string> strike;
    std::optional<std::string> put_call;
};

struct InstrumentPage {
    std::vector<Instrument> items;
    std::optional<std::string> next_cursor;
};

// One venue's listing of a market. The instrument id is what a subscription
// takes; the exchange symbol is what the venue calls it, kept because that is
// what a person recognises and what an order carries.
struct Listing {
    std::string instrument_id;
    std::string account;
    std::string venue;
    std::string market;
    std::string exchange_symbol;
};

// ONE MARKET, everywhere it is listed.
//
// This grouping is the product of the canonical symbol and it is done HERE
// rather than in the browser: every client would otherwise re-derive it, and
// two clients deriving it differently is two answers to "is this the same
// market", which is the question a cross-venue view exists to answer.
struct Market {
    std::string symbol; // canonical: "BTC/USDT"
    std::vector<Listing> listings;
};

// --------------------------------------------------------------- live ----

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
    std::string mark_iv; // a RATIO: 0.2524 is 25.24 %
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
    std::string ts_event;
};

// What the venue's rolling 24-hour ticker says about turnover. Both columns,
// because base volume compares only between venues quoting the same base and
// quote volume is stated in the quote currency, which is not always dollars.
struct Ticker {
    std::string instrument_id;
    std::string volume_24h_base;
    std::string volume_24h_quote;
    std::string ts_event;
};

// ------------------------------------------------------------ trading ----

struct Position {
    std::string instrument_id;
    std::int64_t strategy_id{0};
    std::string qty; // signed; positive is long
    std::string avg_entry_price;
    std::string realized;
    // NULL IS NOT FLAT. A venue that cannot report, a pull that failed and a
    // venue reporting zero are three different facts.
    std::optional<std::string> venue_qty;
    std::optional<std::string> venue_reported_at;
    std::string ts_updated;
};

// A currency the account holds, as the venue last reported it.
//
// NOT a position and not equity. Equity is a sum over these, and a sum over a
// set that is missing a row is a number nobody holds — which is why the whole
// set travels together and carries the instant it was taken.
struct Balance {
    std::string asset;
    // Absent when the venue did not report it. NOT zero: several venues publish
    // a total and no free figure, and repeating the total there would invite a
    // caller to commit margin twice.
    std::optional<std::string> total;
    std::optional<std::string> available;
    std::optional<std::string> unrealized;
};

struct AccountBalances {
    std::string account;
    std::string venue;
    // When the venue last answered. Absent when it has not — and then `assets`
    // is empty because there is nothing to say, never because the account is.
    std::optional<std::string> reported_at;
    bool stale{false};
    std::vector<Balance> assets;
};

// -------------------------------------------------------------- error ----

// RFC 9457. One error shape everywhere, including on the stream.
struct Problem {
    std::string title;
    std::int32_t status{0};
    std::optional<std::string> detail;
};

} // namespace raikiri::admin
