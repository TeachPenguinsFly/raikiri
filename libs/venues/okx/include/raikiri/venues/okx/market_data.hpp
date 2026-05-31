#pragma once

// What the public feed says a market did: its prints, the funding it quotes,
// and the ticker's running picture of both.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/okx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::okx {

// `side` is already the AGGRESSOR's side — OKX states the taker direction
// directly, so nothing is inverted here or in the mapper. Binance's `m` says
// the opposite thing and needs a flip; reading these two the same way is how
// a signed flow series comes out backwards.
struct WireTrade {
    std::string_view inst_id;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty;
    bool aggressor_bought{false};
    std::int64_t seq_id{0};
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

// funding_ms is OKX's `fundingTime`: the settlement the CURRENT rate applies
// to, which is the next settlement in wall-clock terms. `nextFundingTime` is
// the one after that, and reading it as "next funding" puts every countdown
// one interval into the future.
//
// The websocket funding-rate frame and GET /public/funding-rate ship the
// identical object, so one parser reads both.
struct WireFundingRate {
    std::string_view inst_id;
    std::string_view funding_rate;
    std::string_view next_funding_rate; // forecast; observed empty in practice
    std::uint64_t funding_ms{0};
    std::uint64_t next_funding_ms{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame);

// The `tickers` channel, and GET /market/ticker(s) under the same names.
//
// It exists here for the 24-hour turnover alone: bbo-tbt is the book source
// and funding-rate the rate source, and NEITHER states a volume — OKX splits
// across three channels what bybit and gate put on one ticker frame.
//
// WHICH COLUMN IS WHICH DEPENDS ON inst_type, and OKX reuses the same two
// names when the meaning flips:
//   SWAP/FUTURES — vol24h counts CONTRACTS, which compares with nothing, and
//                  volCcy24h is the BASE asset. No quote figure is published.
//   SPOT/MARGIN  — vol24h is the BASE asset and volCcy24h is the QUOTE.
// Reading one rule for both puts a contract count on a turnover board, so the
// mapper branches on inst_type rather than on the field name.
//
// Empty rather than absent is what a quiet instrument sends; either way the
// mapper must publish nothing, never a zero.
struct WireTicker {
    std::string_view inst_id;
    std::string_view inst_type;   // "SWAP" | "SPOT" | ...: decides the two below
    std::string_view vol_24h;     // vol24h
    std::string_view vol_ccy_24h; // volCcy24h
    std::string_view last;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

// Also shared between the mark-price channel and GET /public/mark-price.
struct WireMarkPrice {
    std::string_view inst_id;
    std::string_view mark_price;
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame);

// What the two channels have said SO FAR about one instrument's funding.
//
// OKX SPLITS WHAT `FundingUpdate` FUSES: the rate arrives on `funding-rate`
// and the mark on `mark-price`, and the shm cell is written whole. Publishing
// straight from either frame writes UNDEF over whatever the other one said, so
// the venue keeps both and publishes the merge. A field never stated stays
// UNDEF, which is not zero.
struct FundingMemo {
    FixedE9 rate{};
    FixedE9 predicted{};
    PriceTicks mark{};
    UnixNanos next_funding{};
    std::uint32_t interval_s{0};
};

} // namespace raikiri::venues::okx
