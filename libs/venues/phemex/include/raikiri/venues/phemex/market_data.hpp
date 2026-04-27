#pragma once

#include <raikiri/venues/phemex/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::phemex {

// [ts_ns, "Buy"|"Sell", price, qty]; the side is the taker's.
struct WireRealPrint {
    std::uint64_t ts_ns{0};
    bool taker_bought{false};
    std::string_view price;
    std::string_view qty;
};

struct WireScaledPrint {
    std::uint64_t ts_ns{0};
    bool taker_bought{false};
    std::int64_t price_ep{0};
    std::int64_t qty_ev{0};
};

template <typename Print> struct WireTrades {
    std::string_view symbol;
    bool snapshot{false}; // the backlog sent on subscribe, not the tape
    std::span<const Print> prints;
};

inline constexpr std::size_t k_max_prints = 1024;

template <typename Print> struct PrintBuffer {
    std::array<Print, k_max_prints> prints{};
};

[[nodiscard]] std::expected<WireTrades<WireRealPrint>, WireError>
parse_real_trades(std::string_view frame, PrintBuffer<WireRealPrint>& storage);

[[nodiscard]] std::expected<WireTrades<WireScaledPrint>, WireError>
parse_scaled_trades(std::string_view frame, PrintBuffer<WireScaledPrint>& storage);

// perp_market24h_pack_p rows are positional; only the snapshot names the
// columns, so the order is carried from it to every incremental after.
struct PackColumns {
    int symbol{-1};
    int mark{-1};
    int rate{-1};
    int predicted{-1};
    int volume{-1};
    int turnover{-1};

    [[nodiscard]] bool known() const noexcept { return symbol >= 0; }
};

struct WirePackRow {
    std::string_view symbol;
    std::string_view mark;
    std::string_view rate;      // per the contract's own interval, not per 8 h
    std::string_view predicted; // equal to `rate` on every live row measured
    std::string_view volume;    // base
    std::string_view turnover;  // quote
};

inline constexpr std::size_t k_max_pack_rows = 2048;

struct PackBuffer {
    std::array<WirePackRow, k_max_pack_rows> rows{};
};

struct WirePack {
    std::uint64_t ts_ns{0};
    std::span<const WirePackRow> rows;
};

// Fails with MissingField on rows that arrive before any frame named the columns.
[[nodiscard]] std::expected<WirePack, WireError>
parse_perp_pack(std::string_view frame, PackColumns& columns, PackBuffer& storage);

// market24h, one coin-margined perp per frame, every number scaled.
struct WireInverseTicker {
    std::string_view symbol;
    std::optional<std::int64_t> rate_er;
    std::optional<std::int64_t> predicted_er;
    std::optional<std::int64_t> mark_ep;
    std::optional<std::int64_t> volume; // contracts
    std::optional<std::int64_t> turnover_ev; // the settle coin
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireInverseTicker, WireError>
parse_inverse_ticker(std::string_view frame);

// spot_market24h, one pair per frame.
struct WireSpotTicker {
    std::string_view symbol;
    std::optional<std::int64_t> volume_ev;
    std::optional<std::int64_t> turnover_ev;
    std::uint64_t ts_ns{0};
};

[[nodiscard]] std::expected<WireSpotTicker, WireError> parse_spot_ticker(std::string_view frame);

} // namespace raikiri::venues::phemex
