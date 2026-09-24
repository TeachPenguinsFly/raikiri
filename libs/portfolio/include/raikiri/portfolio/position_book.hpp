#pragma once

// fixed capacity so pointers from `find` stay valid across callbacks

#include <raikiri/domain/instrument.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/portfolio/position.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace raikiri::portfolio {

enum class BookError : std::uint8_t {
    Full, // capacity exhausted; size the book right
};

class PositionBook {
public:
    static constexpr std::size_t k_default_capacity = 256;

    explicit PositionBook(std::size_t capacity = k_default_capacity) : capacity_{capacity} {
        positions_.reserve(capacity);
    }

    [[nodiscard]] Position* find(PositionKey key) noexcept;
    [[nodiscard]] const Position* find(PositionKey key) const noexcept;

    [[nodiscard]] std::expected<Position*, BookError> open(PositionKey key, UnixNanos ts);

    // seats a journaled position on restart; refuses a key already present
    [[nodiscard]] bool restore(PositionKey key, const Position::Restored& state);

    // folds one fill into its position; strategy_id comes from the event's ClientOrderId
    [[nodiscard]] std::expected<Position::Applied, BookError>
    on_fill(const InstrumentBase& base, const OrderEvent& event, QtyLots delta);

    // Insertion order, and stable: the ledger and the tests both lean on it.
    [[nodiscard]] std::span<const Position> all() const noexcept { return positions_; }

    [[nodiscard]] std::size_t size() const noexcept { return positions_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::vector<Position> positions_;
    std::size_t capacity_;
};

} // namespace raikiri::portfolio
