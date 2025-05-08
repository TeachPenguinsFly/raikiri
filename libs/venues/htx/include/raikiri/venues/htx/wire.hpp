#pragma once

// HTX (formerly Huobi) USDT-M swap wire types and parsers.
//
// Market data and funding arrive from two endpoints speaking two dialects —
// `ch`/`sub` on /linear-swap-ws, `op`/`topic` on /linear-swap-notification.
// One EventKind covers both, because `ch` and `op` never appear together.

#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

using WireError = wire::Error;

// Resolved once per frame, so downstream dispatch is an integer compare.
enum class EventKind : std::uint8_t {
    Bbo,        // market.{code}.bbo
    Trade,      // market.{code}.trade.detail
    Depth,      // market.{code}.depth.step0
    Funding,    // op:notify carrying public.{code}.funding_rate
    MarketPing, // {"ping":<ms>}
    NotifyPing, // {"op":"ping","ts":"<ms>"}
    SubscribeAck,
    Error,
    Unknown,
};

struct EventInfo {
    EventKind kind{EventKind::Unknown};
    std::string_view contract_code; // empty on control frames and keepalives

    std::string_view keepalive_ts;
};

[[nodiscard]] std::expected<EventInfo, WireError> parse_event(std::string_view frame);

struct StreamError {
    std::string_view code; // "bad-request" or "2011": see the header note
    std::string_view msg;
};

[[nodiscard]] std::expected<StreamError, WireError> parse_stream_error(std::string_view frame);

// ------------------------------------------------------------ public ----

// `bid`/`ask` are two-element arrays: price then a count of CONTRACTS, never
// base units. tick.ts is the matching-engine time; the root `ts` is when HTX
// sent the frame.
struct WireBbo {
    std::string_view contract_code;
    std::string_view bid_price;
    std::string_view bid_qty;
    std::string_view ask_price;
    std::string_view ask_qty;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
};

[[nodiscard]] std::expected<WireBbo, WireError> parse_bbo(std::string_view frame);

// `direction` is already the AGGRESSOR's side — HTX names the taker directly,
// so nothing is inverted here or in the mapper. bingx's `m` says the opposite
// thing, and reading the two the same way is how a signed flow series comes
// out backwards.
//
// `amount` is contracts and `quantity` the same trade in base units. Only
// amount is mapped: bbo and depth quote contracts too.
struct WireTrade {
    std::string_view contract_code;
    std::string_view price;
    std::string_view amount;
    std::string_view quantity;
    bool aggressor_bought{false};
    std::uint64_t trade_id{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

struct WireLevel {
    std::string_view price;
    std::string_view qty;
};

// step0 is the unaggregated book and ships about 150 levels a side; the
// ceiling leaves room without letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_levels = 512;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

// A complete snapshot every time, with no delta chain and so no continuity to
// validate. `version` equals tick.id on step0 and tick.mrid on the aggregated
// levels, which is why it is carried rather than trusted as a sequence.
//
// RE-VERIFIED 2026-08-17 on BTC-USDT: 119 consecutive step0 frames, every one
// of them 150 levels a side with no action field anywhere. `version` and `id`
// are epoch SECONDS, so ten frames a second share one value; `mrid` is the
// matching-engine id and is the only token here that orders frames.
struct WireDepth {
    std::string_view contract_code;
    std::uint64_t ts_ms{0};
    std::uint64_t version{0};
    std::uint64_t mrid{0};
    std::span<const WireLevel> bids; // descending
    std::span<const WireLevel> asks; // ascending
};

[[nodiscard]] std::expected<WireDepth, WireError> parse_depth(std::string_view frame,
                                                              LevelBuffer& storage);

// The websocket funding notification. `settlement_time` is the next
// settlement; `funding_time` is when HTX built the message and moves every few
// seconds. GET /swap_funding_rate gives `funding_time` the OTHER meaning, so
// the two surfaces name different instants alike — hence only the socket shape
// is parsed here. The rate is a STRING of eighteen fractional digits whose
// tail is not always zero ("-0.000058784590419109" live on ETH-USDT).
struct WireFunding {
    std::string_view contract_code;
    std::string_view funding_rate;
    std::string_view estimated_rate; // JSON null in every frame observed
    std::uint64_t settlement_ms{0};
    std::uint64_t funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

// ------------------------------------------------------------- rest ----

// `status` is "ok" or "error"; there is no success code to compare against.
// A body that carries neither — HTX answers an unrouted path with a Spring
// error whose `status` is the NUMBER 404 — fails to parse rather than
// reporting success, so the caller falls back to the HTTP status.
struct RestEnvelope {
    bool ok{false};
    std::string_view code; // empty when the venue named none
    std::string_view msg;
};

[[nodiscard]] std::expected<RestEnvelope, WireError> parse_rest_envelope(std::string_view body);

// contract_size is what one contract is worth in base units, and the only
// quantity unit HTX publishes: an order is an integer count of contracts, so
// there is no separate step. settlement_period is the funding interval in
// HOURS as a digit string ("8", "4" and "1" are all live), which okx has to
// infer from consecutive funding frames.
struct WireContract {
    std::string_view contract_code;
    std::string_view symbol;
    std::string_view contract_size;
    std::string_view price_tick;
    std::string_view business_type; // "swap" on every row this endpoint serves
    std::string_view contract_type;
    std::string_view delivery_time; // empty on a swap, an epoch on a delisting
    std::int64_t contract_status{0};
    std::int64_t settlement_period_h{0};
    std::uint64_t settlement_ms{0};
};

inline constexpr std::size_t k_max_instruments = 1024; // 276 rows live today

struct ContractBuffer {
    std::array<WireContract, k_max_instruments> contracts{};
};

[[nodiscard]] std::expected<std::span<const WireContract>, WireError>
parse_contracts(std::string_view body, ContractBuffer& storage);

// GET /api/v1/timestamp answers {"status":"ok","ts":<ms>} with no `data` at
// all, so this reads the envelope's own timestamp.
[[nodiscard]] std::expected<std::uint64_t, WireError> parse_server_time(std::string_view body);

} // namespace raikiri::venues::htx
