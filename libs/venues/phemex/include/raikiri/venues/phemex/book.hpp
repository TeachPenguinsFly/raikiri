#pragma once

#include <raikiri/book/ladder.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/market_data.hpp>
#include <raikiri/venues/phemex/errors.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace raikiri::venues::phemex {

// orderbook_p (USDT/USDC perps): decimal strings.
struct WireRealLevel {
    std::string_view price;
    std::string_view qty; // "0" deletes
};

// book (spot and coin-margined perps): scaled integers.
struct WireScaledLevel {
    std::int64_t price_ep{0};
    std::int64_t qty_ev{0}; // 0 deletes
};

struct WireBookHead {
    std::string_view symbol;
    bool snapshot{false};
    std::uint32_t depth{0};
    std::uint64_t sequence{0};
    std::uint64_t ts_ns{0};
};

template <typename Level> struct WireBook {
    WireBookHead head;
    std::span<const Level> bids;
    std::span<const Level> asks;
};

inline constexpr std::size_t k_max_levels = 1024;

template <typename Level> struct LevelBuffer {
    std::array<Level, k_max_levels> bids{};
    std::array<Level, k_max_levels> asks{};
};

[[nodiscard]] std::expected<WireBook<WireRealLevel>, WireError>
parse_real_book(std::string_view frame, LevelBuffer<WireRealLevel>& storage);

[[nodiscard]] std::expected<WireBook<WireScaledLevel>, WireError>
parse_scaled_book(std::string_view frame, LevelBuffer<WireScaledLevel>& storage);

enum class BookResult : std::uint8_t {
    Replaced, // a snapshot replaced the book
    Applied,
    Ignored, // an update with no book under it
    Crossed, // the top crossed: book dropped until the next snapshot
};

// sequence spans every symbol and there is no checksum; the 60 s snapshot replaces the book
class Book {
public:
    using Level = book::Level;

    explicit Book(InstrumentId instrument) : instrument_{instrument} {}

    // depth is the venue's window; levels outside it are never updated, so the book is cut to it
    BookResult apply(bool snapshot,
                     std::span<const Level> bids,
                     std::span<const Level> asks,
                     std::uint32_t depth,
                     std::uint64_t sequence);

    [[nodiscard]] bool synced() const noexcept { return synced_; }

    [[nodiscard]] std::optional<Bbo> top(UnixNanos ts_event, UnixNanos ts_init) const;

    [[nodiscard]] std::optional<BookSnapshot> snapshot(UnixNanos ts_event, UnixNanos ts_init) const;

    void invalidate() noexcept;

    [[nodiscard]] std::uint64_t replacements() const noexcept { return replacements_; }

    [[nodiscard]] std::uint64_t crossings() const noexcept { return crossings_; }

    [[nodiscard]] std::span<const Level> bids() const noexcept { return ladders_.bids().levels(); }

    [[nodiscard]] std::span<const Level> asks() const noexcept { return ladders_.asks().levels(); }

private:
    InstrumentId instrument_{};
    book::Ladders ladders_;
    std::uint64_t sequence_{0};
    std::uint64_t replacements_{0};
    std::uint64_t crossings_{0};
    bool synced_{false};
};

} // namespace raikiri::venues::phemex
