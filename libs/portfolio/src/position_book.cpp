#include <raikiri/portfolio/position_book.hpp>

#include <algorithm>

namespace raikiri::portfolio {

Position* PositionBook::find(PositionKey key) noexcept {
    const auto it =
        std::ranges::find_if(positions_, [key](const Position& p) { return p.key() == key; });
    return it == positions_.end() ? nullptr : &*it;
}

const Position* PositionBook::find(PositionKey key) const noexcept {
    const auto it =
        std::ranges::find_if(positions_, [key](const Position& p) { return p.key() == key; });
    return it == positions_.end() ? nullptr : &*it;
}

std::expected<Position*, BookError> PositionBook::open(PositionKey key, UnixNanos ts) {
    if (auto* existing = find(key); existing != nullptr) {
        return existing;
    }
    if (positions_.size() >= capacity_) {
        return std::unexpected{BookError::Full};
    }
    positions_.emplace_back(key, ts);
    return &positions_.back();
}

std::expected<Position::Applied, BookError>
PositionBook::on_fill(const InstrumentBase& base, const OrderEvent& event, QtyLots delta) {
    const PositionKey key{event.client_order_id.strategy_id(), event.instrument};
    auto position = open(key, event.ts_event);
    if (!position.has_value()) {
        return std::unexpected{position.error()};
    }
    return (*position)->apply_fill(base, event.side, delta, event.price, event.fee, event.ts_event);
}

bool PositionBook::restore(PositionKey key, const Position::Restored& state) {
    if (find(key) != nullptr || positions_.size() >= capacity_) {
        return false;
    }
    positions_.emplace_back(key, state);
    return true;
}

} // namespace raikiri::portfolio
