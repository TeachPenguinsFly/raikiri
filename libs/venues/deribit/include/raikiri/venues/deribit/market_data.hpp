#pragma once

// What the public feed says a market did: its prints, and the one ticker
// frame that carries the mark, the funding and an option's greeks together.

#include <raikiri/venues/deribit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

// `direction` is already the AGGRESSOR's side, so nothing is inverted here or
// in the mapper. `amount` is the venue's own quantity unit — USD on the
// reversed futures, coin contracts on options.
struct WireTrade {
    std::string_view instrument;
    std::string_view trade_id;
    std::string_view price;
    std::string_view qty; // `amount`
    bool aggressor_bought{false};
    std::int64_t trade_seq{0};
    std::uint64_t ts_ms{0};
};

inline constexpr std::size_t k_max_trades = 128;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError>
parse_trades(std::string_view frame, TradeBuffer& storage);

// One frame carries mark price and funding, so unlike OKX there is no separate
// mark channel to correlate. current_funding is the instantaneous rate and is
// legitimately 0.0 (captured live), so absence must be decided by the FIELD
// being missing — a dated future ships no funding fields at all. funding_8h is
// the trailing realised average: read so a schema change stays visible, but
// deliberately not mapped.
struct WireTicker {
    std::string_view instrument;
    std::string_view mark_price;
    std::string_view current_funding; // empty on anything but a perpetual
    std::string_view funding_8h;      // empty on anything but a perpetual
    // Empty on anything but an option. Deribit computes these itself and ships
    // them on the same ticker as the mark, so an options desk gets the venue's
    // own analytics rather than ours — see OptionGreeks for why that matters.
    // The implied vols arrive as PERCENTS here; the conversion to a ratio is
    // the mapper's, at the one edge where units change.
    std::string_view delta;
    std::string_view gamma;
    std::string_view vega;
    std::string_view theta;
    std::string_view rho;
    std::string_view mark_iv;
    std::string_view bid_iv;
    std::string_view ask_iv;
    std::string_view underlying_price;
    // The `stats` sub-object's rolling 24-hour turnover. `volume` is the BASE
    // asset — BTC on every BTC-quoted contract, option contracts included —
    // and `volume_usd` the same window in USD, which is the quote currency of
    // every instrument this adapter loads. Being coin-margined does NOT swap
    // them: verified by ratio in map.cpp.
    //
    // Optional because a ticker without `stats` must still yield its funding
    // rate; every live frame has carried it.
    std::string_view stats_volume;     // base units
    std::string_view stats_volume_usd; // quote units
    std::uint64_t ts_ms{0};
};

// True when the frame carried an option's analytics. A perpetual's ticker has
// no `greeks` object at all, so this separates "not an option" from "an option
// whose greeks are genuinely zero" — deep in the money, they are.
[[nodiscard]] constexpr bool has_greeks(const WireTicker& wire) noexcept {
    return !wire.delta.empty() || !wire.mark_iv.empty();
}

[[nodiscard]] std::expected<WireTicker, WireError> parse_ticker(std::string_view frame);

} // namespace raikiri::venues::deribit
