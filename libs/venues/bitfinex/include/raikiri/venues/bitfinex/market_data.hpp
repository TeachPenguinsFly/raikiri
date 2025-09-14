#pragma once

// What the public channels say a market did: its ticker, its prints, and the
// perpetual's own funding and mark status.
//
// Every payload here is a POSITIONAL array. The layout comment above each
// struct is the schema — there are no field names on the wire to fall back on.

#include <raikiri/venues/bitfinex/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bitfinex {

// ticker payload: [BID,BID_SIZE,ASK,ASK_SIZE,DAILY_CHANGE,DAILY_CHANGE_REL,
//                  LAST_PRICE,VOLUME,HIGH,LOW]
//
// Bitfinex documents ten fields and sends ELEVEN on both spot and the
// perpetual (verified live 2026-08-11); the extra trailing element is `null`
// over the websocket and an unexplained integer over REST. The arity below is
// therefore what is READ, not what arrives — requiring the documented ten
// exactly would have been correct and would still have refused every live
// frame. Tests pin the observed count so a further append is visible.
inline constexpr std::size_t k_ticker_arity = 10;
inline constexpr std::size_t k_ticker_bid = 0;
inline constexpr std::size_t k_ticker_bid_size = 1;
inline constexpr std::size_t k_ticker_ask = 2;
inline constexpr std::size_t k_ticker_ask_size = 3;
inline constexpr std::size_t k_ticker_last_price = 6;
inline constexpr std::size_t k_ticker_volume = 7;

// No timestamp anywhere in the payload, so the mapper has only ingest time.
struct WireTicker {
    std::string_view bid;
    std::string_view bid_size;
    std::string_view ask;
    std::string_view ask_size;
    std::string_view last_price;
    // The 24-hour VOLUME slot, in the BASE asset. There is no quote column on
    // this channel at all — no turnover, no notional, nothing to pair it with
    // — so the quote side of TickerStats stays a hard zero rather than being
    // manufactured by multiplying this by a price.
    //
    // Positional, and never absent: read_row refuses a short array outright,
    // so a frame either states all ten slots or is not a ticker.
    std::string_view volume;
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// trade payload: [ID,MTS,AMOUNT,PRICE]
//
// AMOUNT IS SIGNED AND THE SIGN IS THE AGGRESSOR: positive means the taker
// bought. There is no side field to cross-check it against. Verified live —
// in one capture every negative print landed on the bid (63945/63951 against
// a 63951 bid) and every positive one at or above the ask. Read backwards,
// signed flow comes out exactly inverted and nothing errors.
inline constexpr std::size_t k_trade_arity = 4;
inline constexpr std::size_t k_trade_id = 0;
inline constexpr std::size_t k_trade_mts = 1;
inline constexpr std::size_t k_trade_amount = 2;
inline constexpr std::size_t k_trade_price = 3;

struct WireTrade {
    std::string_view id;
    std::string_view amount; // signed; see above
    std::string_view price;
    std::uint64_t ts_ms{0};
};

// A subscribe snapshot carries thirty prints; the ceiling leaves room without
// letting a hostile frame make us allocate.
inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// `te` and `tu` are the SAME print twice: Bitfinex emits the execution
// immediately and restates it once the trade id is final. Both were observed
// carrying identical id/mts/amount/price, so a consumer that publishes both
// double-counts every trade on the venue.
enum class TradeFrameKind : std::uint8_t {
    Snapshot, // the array-of-arrays that follows a subscribe: history
    Executed, // "te"
    Updated,  // "tu"
};

struct WireTrades {
    TradeFrameKind kind{TradeFrameKind::Executed};
    std::span<const WireTrade> trades;
};

[[nodiscard]] std::expected<WireTrades, WireError> parse_trades(std::string_view frame,
                                                                TradeBuffer& storage);

// deriv status payload, 23 elements of which fifteen are `null` placeholders.
// This is the funding/mark channel for perpetuals, subscribed by
// `key: "deriv:tBTCF0:USTF0"`.
//
// MARK_PRICE (14) and DERIV_PRICE (2) are different numbers and confusing them
// is the expensive mistake: DERIV_PRICE is the perpetual's own mid (63969.45
// against a 63962/63972 book) while MARK_PRICE tracks the index (63939.37
// against a 63938 spot) and is what liquidations use.
inline constexpr std::size_t k_deriv_arity = 23;
inline constexpr std::size_t k_deriv_mts = 0;
inline constexpr std::size_t k_deriv_price = 2;
inline constexpr std::size_t k_deriv_spot_price = 3;
inline constexpr std::size_t k_deriv_next_funding_mts = 7;
inline constexpr std::size_t k_deriv_next_funding_accrued = 8;
inline constexpr std::size_t k_deriv_next_funding_step = 9;
inline constexpr std::size_t k_deriv_current_funding = 11;
inline constexpr std::size_t k_deriv_mark_price = 14;
inline constexpr std::size_t k_deriv_open_interest = 17;

struct WireDerivStatus {
    std::string_view deriv_price;
    std::string_view spot_price;
    std::string_view mark_price;
    std::string_view current_funding;      // rate being applied now
    std::string_view next_funding_accrued; // running estimate for the next event
    std::string_view open_interest;
    std::uint64_t ts_ms{0};
    std::uint64_t next_funding_ms{0};
};

[[nodiscard]] std::expected<WireDerivStatus, WireError> parse_deriv_status(std::string_view frame);

} // namespace raikiri::venues::bitfinex
