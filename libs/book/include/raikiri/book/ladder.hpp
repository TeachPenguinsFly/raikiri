#pragma once

#include <raikiri/domain/market_data.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace raikiri::book {

struct Level {
    PriceTicks price{};
    QtyLots qty{};
};

enum class Side : std::uint8_t { Bid, Ask };

// bids descend, asks ascend; the touch is element zero
class Ladder {
public:
    explicit Ladder(Side side) noexcept : side_{side} {}

    // a zero quantity is not a delete here; the caller calls remove()
    void upsert(Level level);

    void remove(PriceTicks price);

    void clear() noexcept { levels_.clear(); }

    void reserve(std::size_t capacity) { levels_.reserve(capacity); }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    [[nodiscard]] std::span<const Level> levels() const noexcept { return levels_; }

    void truncate(std::size_t depth);

    void drop_beyond(PriceTicks worst);

private:
    [[nodiscard]] bool precedes(PriceTicks a, PriceTicks b) const noexcept {
        return side_ == Side::Bid ? a.raw > b.raw : a.raw < b.raw;
    }

    Side side_;
    std::vector<Level> levels_;
};

// a one-sided or untrusted book publishes as nothing
class Ladders {
public:
    Ladders() : bids_{Side::Bid}, asks_{Side::Ask} {}

    [[nodiscard]] Ladder& bids() noexcept { return bids_; }

    [[nodiscard]] Ladder& asks() noexcept { return asks_; }

    [[nodiscard]] const Ladder& bids() const noexcept { return bids_; }

    [[nodiscard]] const Ladder& asks() const noexcept { return asks_; }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
    }

    [[nodiscard]] std::optional<std::pair<Level, Level>> touch() const noexcept;

    [[nodiscard]] std::optional<BookSnapshot> snapshot(InstrumentId instrument,
                                                       std::uint64_t venue_seq,
                                                       UnixNanos ts_event,
                                                       UnixNanos ts_init) const;

private:
    Ladder bids_;
    Ladder asks_;
};

// a value off the step means the venue changed a filter; a delta book must then be invalidated
[[nodiscard]] constexpr std::optional<std::int64_t> exact_steps(FixedE9 value,
                                                                FixedE9 step) noexcept {
    if (step.raw <= 0 || value.raw % step.raw != 0) {
        return std::nullopt;
    }
    return value.raw / step.raw;
}

} // namespace raikiri::book
