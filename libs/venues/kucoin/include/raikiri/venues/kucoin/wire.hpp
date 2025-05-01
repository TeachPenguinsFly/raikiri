#pragma once

// KuCoin Futures wire types and parsers.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::kucoin {

enum class WireError : std::uint8_t {
    NotJson,
    MissingField,
    WrongType,
    TooManyEntries,
};

[[nodiscard]] std::string_view to_string(WireError err) noexcept;

// Resolved from type/subject once, so downstream dispatch is an integer
// compare rather than a string compare per message.
enum class EventKind : std::uint8_t {
    Bbo,         // subject tickerV2
    Trade,       // subject match, on /contractMarket/execution
    Depth,       // subject level2
    MarkPrice,   // subject mark.index.price, on /contract/instrument
    FundingRate, // subject funding.rate, on the same topic
    Welcome,     // sent once on connect, before anything may be subscribed
    Ack,
    Pong,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view symbol; // empty on control frames
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

// A refused subscription or a rejected token. `data` holds the text here,
// where a REST body would use `msg`.
struct StreamError {
    std::int64_t code{0};
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// Sizes are CONTRACTS and arrive as bare integers; they still travel as text so
// the mapper divides by the instrument's lot step rather than assuming the
// lotSize of 1 that KuCoin happens to list on every contract today.
struct WireBbo {
    std::string_view symbol;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `side` is already the AGGRESSOR's side — KuCoin states the taker direction
// directly, so nothing is inverted here or in the mapper. Binance's `m` says the
// opposite thing, and reading the two alike is how signed flow comes out backwards.
struct WireTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireTrade, WireError> parse_trade(std::string_view frame);

// One level2 frame is ONE level packed into a comma-joined string:
// "62234.6,buy,1" is price, side, size, and a size of 0 deletes the level. The
// stream carries no snapshot; a book must start from GET /api/v1/level2/snapshot
// and replay from `sequence`.
struct WireBookChange {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    bool bid{false};
    std::int64_t sequence{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame);

struct WireMarkPrice {
    std::string_view symbol;
    std::string_view mark_price;
    std::string_view index_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// The websocket form carries no settlement time; the REST form does but names
// the funding INDEX (".XBTUSDTMFPI8H") in `symbol`, useless for lookup — hence
// one DTO, two parsers, and a `symbol` only the websocket form fills.
//
// `granularity` means DIFFERENT THINGS on the two transports, so the mapper
// trusts neither: the stream republishes the live rate once a minute and says
// 60000, while REST says the 28800000 settlement period (verified live).
struct WireFundingRate {
    std::string_view symbol;
    std::string_view rate;
    std::string_view predicted_rate; // REST `predictedValue`; absent in practice
    std::uint64_t granularity_ms{0};
    std::uint64_t next_funding_ms{0}; // REST `fundingTime`; 0 on the websocket
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// ------------------------------------------------------------- rest ----

// "200000" is success. The code is a string, so it is converted once here and
// everything below works in integers.
inline constexpr std::int64_t k_ok = 200'000;

struct RestEnvelope {
    std::int64_t code{0};
    std::string_view msg;
};

// Fails when `code` is missing or is not a run of digits: a body whose envelope
// cannot be read must never be mistaken for one that said "200000".
[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// POST /api/v1/bullet-public: a short-lived token plus the endpoint to use it
// against. Part of connecting rather than of startup, because it expires.
struct WireBullet {
    std::string_view token;
    std::string_view endpoint; // "wss://ws-api-futures.kucoin.com/"
    std::uint64_t ping_interval_ms{0};
    std::uint64_t ping_timeout_ms{0};
};

[[nodiscard]] std::expected<WireBullet, WireError> parse_bullet(std::string_view body);

// `multiplier` is how many base units one CONTRACT is worth, and it is negative
// on inverse contracts, whose size is quoted in the quote currency instead; the
// mapper refuses those rather than storing a negative size.
struct WireContract {
    std::string_view symbol;
    std::string_view status; // "Open"
    std::string_view type;   // FFWCSX perpetual, FFICSX dated
    std::string_view tick_size;
    std::string_view lot_size;
    std::string_view multiplier;
    std::string_view index_tick_size;
    std::string_view max_order_qty;
    std::uint64_t funding_granularity_ms{0};
};

inline constexpr std::size_t k_max_contracts = 2048;

struct ContractBuffer {
    std::array<WireContract, k_max_contracts> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /api/v1/funding-rate/{symbol}/current. Seeds funding at subscribe time and
// is the only source that states the venue's own next settlement.
[[nodiscard]] std::expected<WireFundingRate, WireError>
parse_funding_current(std::string_view body);

// GET /api/v1/timestamp. `data` is a bare number here, not an object.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

} // namespace raikiri::venues::kucoin
