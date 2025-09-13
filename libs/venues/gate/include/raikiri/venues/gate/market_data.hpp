#pragma once

// What the public feed says a market did: its prints, and the ticker that
// carries funding, mark price and turnover on one frame.

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/gate/errors.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::gate {

// `qty` is the MAGNITUDE of Gate's signed `size`; the sign became
// aggressor_bought. Reading `size` unsigned loses the taker direction, which
// is how a signed flow series comes out flat.
struct WireTrade {
    std::string_view contract;
    std::string_view qty;
    std::string_view price;
    std::int64_t trade_id{0};
    bool aggressor_bought{false};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

// One frame can carry several prints, so this returns a span where the other
// channels return one struct.
[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage, MarketType market);

// futures.tickers is the funding channel AND the mark-price channel: one
// subscription carries both, unlike okx. The interval is stated outright, so
// nothing has to be derived from two settlement times.
//
// `funding_rate_indicative` is DEPRECATED per Gate's own SDK ("use
// funding_rate") and was observed equal to it on every frame, so it is
// surfaced but deliberately not mapped to a forecast.
struct WireTicker {
    std::string_view contract;
    std::string_view funding_rate;
    std::string_view funding_rate_indicative;
    std::string_view mark_price;
    std::string_view index_price;
    // The base and quote columns by name. `volume_24h` alone is in
    // CONTRACTS on this venue, which compares with nothing.
    std::string_view volume_24h_base;
    std::string_view volume_24h_quote;
    std::uint64_t funding_next_apply_s{0};
    std::uint32_t funding_interval_s{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

} // namespace raikiri::venues::gate
