#pragma once

// What the public feed says a market did: its prints, the rolling 24-hour
// window, and the funding the swap quotes. All three are swap topics; HTX
// publishes no spot equivalent this adapter reads.

#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

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

// market.{code}.detail — the rolling 24-hour window, and the only push HTX
// has for it: the funding notification states no turnover, and neither does
// bbo, trade.detail or depth.step0.
//
// THREE volume figures arrive on this tick and only two of them compare with
// anything. `amount` is the window in BASE units and `trade_turnover` the same
// window in QUOTE; `vol` is the same trade count in CONTRACTS — VERIFIED live
// on BTC-USDT, vol 8266024 against amount 8266.024 at a contract_size of
// 0.001 — and is deliberately not surfaced, because a contract is a different
// quantity on every row of the catalogue.
//
// A whole snapshot every push, not a delta. Both columns are optional anyway:
// empty is what a contract with nothing to report would leave here, and empty
// is not zero.
struct WireDetail {
    std::string_view contract_code;
    std::string_view amount;         // base units
    std::string_view trade_turnover; // quote units
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireDetail, WireError> parse_detail(std::string_view frame);

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

} // namespace raikiri::venues::htx
