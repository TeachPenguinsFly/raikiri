#pragma once

// seq counts frames on the socket, across all its markets

#include <raikiri/venues/extended/catalogue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::extended {

struct WireLevel {
    std::string_view price;
    std::string_view size; // absolute; "0" deletes the level
};

// room above the deepest snapshot side
inline constexpr std::size_t k_max_levels = 8192;

struct LevelBuffer {
    std::array<WireLevel, k_max_levels> bids{};
    std::array<WireLevel, k_max_levels> asks{};
};

struct WireBook {
    std::string_view market;
    bool snapshot{false};
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0};
    std::span<const WireLevel> bids;
    std::span<const WireLevel> asks;
};

// a delta's q is the change and c the absolute size; a snapshot has only q, absolute
[[nodiscard]] std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage);

struct WirePrint {
    std::string_view market;
    std::uint64_t id{0};
    bool taker_bought{false};
    std::uint64_t ts_ms{0};
    std::string_view price;
    std::string_view qty;
};

inline constexpr std::size_t k_max_prints = 512;

struct PrintBuffer {
    std::array<WirePrint, k_max_prints> prints{};
};

struct WirePrints {
    std::uint64_t seq{0};
    std::span<const WirePrint> prints;
};

[[nodiscard]] std::expected<WirePrints, WireError> parse_trades(std::string_view frame, PrintBuffer& storage);

struct WireMark {
    std::string_view market;
    std::string_view price;
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0}; // the frame's: data.ts is always 0
};

[[nodiscard]] std::expected<WireMark, WireError> parse_mark(std::string_view frame);

struct WireFunding {
    std::string_view market;
    std::string_view rate; // per hour, recomputed every minute
    std::uint64_t seq{0};
    std::uint64_t ts_ms{0};
};

[[nodiscard]] std::expected<WireFunding, WireError> parse_funding(std::string_view frame);

} // namespace raikiri::venues::extended
