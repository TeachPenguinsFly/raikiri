#include <raikiri/simcore/book.hpp>

#include <algorithm>

namespace raikiri::simcore {

namespace {

// Walks the far side price-then-time. Fills happen at RESTING prices — the
// aggressor pays what the book displays. Fully consumed makers leave both the
// level queue and the id index so the two structures never diverge.
template <typename SideMap, typename IndexMap, typename Crosses>
[[nodiscard]] std::int64_t consume(SideMap& far, IndexMap& index, PriceTicks limit,
                                   std::int64_t qty, std::vector<Fill>& fills, Crosses crosses) {
    std::int64_t left = qty;
    while (left > 0 && !far.empty()) {
        const auto level_it = far.begin();
        if (!crosses(level_it->first, limit)) {
            break;
        }
        auto& queue = level_it->second;
        while (left > 0 && !queue.empty()) {
            auto& maker = queue.front();
            const std::int64_t take = std::min(left, maker.leaves.raw);
            maker.leaves.raw -= take;
            left -= take;
            fills.push_back(
                Fill{maker.id, maker.owner, maker.price, QtyLots::from_raw(take), maker.leaves});
            if (maker.leaves.raw == 0) {
                index.erase(maker.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) {
            far.erase(level_it);
        }
    }
    return left;
}

// FOK pre-scan: how much could fill within the limit, capped at `cap` so the
// walk stops as soon as the answer is "enough".
template <typename SideMap, typename Crosses>
[[nodiscard]] std::int64_t liquidity_up_to(const SideMap& far, PriceTicks limit, std::int64_t cap,
                                           Crosses crosses) {
    std::int64_t sum = 0;
    for (const auto& [price, queue] : far) {
        if (!crosses(price, limit)) {
            break;
        }
        for (const auto& order : queue) {
            sum = detail::checked_add(sum, order.leaves.raw);
            if (sum >= cap) {
                return sum;
            }
        }
    }
    return sum;
}

template <typename SideMap>
[[nodiscard]] bool erase_resting(SideMap& side_map, PriceTicks price, std::uint64_t id) {
    const auto level_it = side_map.find(price);
    if (level_it == side_map.end()) {
        return false;
    }
    auto& queue = level_it->second;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->id == id) {
            queue.erase(it);
            if (queue.empty()) {
                side_map.erase(level_it); // empty levels disappear from the book
            }
            return true;
        }
    }
    return false;
}

template <typename SideMap>
[[nodiscard]] Book::RestingOrder* find_in(SideMap& side_map, PriceTicks price, std::uint64_t id) {
    const auto level_it = side_map.find(price);
    if (level_it == side_map.end()) {
        return nullptr;
    }
    for (auto& order : level_it->second) {
        if (order.id == id) {
            return &order;
        }
    }
    return nullptr;
}

[[nodiscard]] QtyLots level_qty(const std::deque<Book::RestingOrder>& queue) noexcept {
    std::int64_t sum = 0;
    for (const auto& order : queue) {
        sum = detail::checked_add(sum, order.leaves.raw);
    }
    return QtyLots::from_raw(sum);
}

// A buy crosses ask levels at or below its limit; a sell crosses bid levels
// at or above. Market orders take every level.
constexpr auto k_buy_crosses = [](PriceTicks level, PriceTicks limit) noexcept {
    return level <= limit;
};
constexpr auto k_sell_crosses = [](PriceTicks level, PriceTicks limit) noexcept {
    return level >= limit;
};
constexpr auto k_any_price = [](PriceTicks, PriceTicks) noexcept { return true; };

} // namespace

MatchResult Book::submit_limit(std::uint64_t id, std::uint32_t owner, Side side, PriceTicks price,
                               QtyLots qty, bool post_only, TimeInForce tif) {
    MatchResult out;
    out.remainder = qty;

    const bool would_cross = side == Side::Buy
                                 ? (!asks_.empty() && asks_.begin()->first <= price)
                                 : (!bids_.empty() && bids_.begin()->first >= price);

    if (post_only && would_cross) {
        // Post-only exists to guarantee maker economics; venues refuse the
        // order rather than let it take. Nothing fills, book is untouched.
        out.disposition = Disposition::RejectedPostOnly;
        return out;
    }

    if (tif == TimeInForce::Fok) {
        // All-or-nothing: if the crossable liquidity cannot cover the full
        // size, nothing trades at all (venue FOK semantics).
        const std::int64_t avail = side == Side::Buy
                                       ? liquidity_up_to(asks_, price, qty.raw, k_buy_crosses)
                                       : liquidity_up_to(bids_, price, qty.raw, k_sell_crosses);
        if (avail < qty.raw) {
            out.disposition = Disposition::Expired;
            return out;
        }
    }

    const std::int64_t left = side == Side::Buy
                                  ? consume(asks_, index_, price, qty.raw, out.fills, k_buy_crosses)
                                  : consume(bids_, index_, price, qty.raw, out.fills,
                                            k_sell_crosses);
    out.remainder = QtyLots::from_raw(left);

    if (left == 0) {
        out.disposition = Disposition::Filled;
        return out;
    }
    if (tif != TimeInForce::Gtc) {
        // IOC remainder never rests (FOK cannot reach here with left > 0 —
        // the pre-scan guaranteed full coverage).
        out.disposition = Disposition::Expired;
        return out;
    }

    Level& level = side == Side::Buy ? bids_[price] : asks_[price];
    level.push_back(RestingOrder{id, owner, side, price, out.remainder, next_seq_++});
    index_.emplace(id, Locator{side, price});
    out.disposition = Disposition::Rested;
    return out;
}

MatchResult Book::submit_market(std::uint64_t /*id*/, std::uint32_t /*owner*/, Side side,
                                QtyLots qty) {
    // Taker identity is accepted for signature symmetry but not stored: the
    // sim does not model self-trade prevention, and a market order never
    // rests, so there is nothing to index.
    MatchResult out;
    const std::int64_t left =
        side == Side::Buy
            ? consume(asks_, index_, PriceTicks::undef(), qty.raw, out.fills, k_any_price)
            : consume(bids_, index_, PriceTicks::undef(), qty.raw, out.fills, k_any_price);
    out.remainder = QtyLots::from_raw(left);
    out.disposition = left == 0 ? Disposition::Filled : Disposition::Expired;
    return out;
}

bool Book::cancel(std::uint64_t id) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    const Locator loc = it->second;
    index_.erase(it);
    return loc.side == Side::Buy ? erase_resting(bids_, loc.price, id)
                                 : erase_resting(asks_, loc.price, id);
}

std::expected<MatchResult, BookError> Book::amend(std::uint64_t id, PriceTicks new_price,
                                                  QtyLots new_leaves) {
    if (new_price.is_undef() || new_price.raw <= 0 || new_leaves.is_undef() ||
        new_leaves.raw <= 0) {
        return std::unexpected{BookError::BadQty};
    }
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return std::unexpected{BookError::UnknownOrder};
    }
    const Locator loc = it->second;
    RestingOrder* order = loc.side == Side::Buy ? find_in(bids_, loc.price, id)
                                                : find_in(asks_, loc.price, id);
    if (order == nullptr) {
        return std::unexpected{BookError::UnknownOrder};
    }

    if (new_price == order->price && new_leaves <= order->leaves) {
        // Venue-real: shrinking in place cannot disadvantage anyone queued
        // behind you, so exchanges preserve time priority for pure size-downs.
        order->leaves = new_leaves;
        MatchResult out;
        out.disposition = Disposition::Rested;
        out.remainder = new_leaves;
        return out;
    }

    // Venue-real: a price change (or size-up) is a replace — the order loses
    // time priority and re-enters as a fresh arrival, which may cross and
    // trade immediately, exactly like an aggressive amend on a live venue.
    const std::uint32_t owner = order->owner;
    const Side side = order->side;
    cancel(id);
    return submit_limit(id, owner, side, new_price, new_leaves, /*post_only=*/false,
                        TimeInForce::Gtc);
}

std::optional<Book::BboLevels> Book::bbo() const {
    if (bids_.empty() && asks_.empty()) {
        return std::nullopt;
    }
    BboLevels out{}; // Scaled64 defaults are undef: empty sides read as undef
    if (!bids_.empty()) {
        const auto& [price, queue] = *bids_.begin();
        out.bid = price;
        out.bid_qty = level_qty(queue);
    }
    if (!asks_.empty()) {
        const auto& [price, queue] = *asks_.begin();
        out.ask = price;
        out.ask_qty = level_qty(queue);
    }
    return out;
}

const Book::RestingOrder* Book::find(std::uint64_t id) const noexcept {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return nullptr;
    }
    const auto scan = [id](const auto& side_map, PriceTicks price) -> const RestingOrder* {
        const auto level_it = side_map.find(price);
        if (level_it == side_map.end()) {
            return nullptr;
        }
        for (const auto& order : level_it->second) {
            if (order.id == id) {
                return &order;
            }
        }
        return nullptr;
    };
    return it->second.side == Side::Buy ? scan(bids_, it->second.price)
                                        : scan(asks_, it->second.price);
}

} // namespace raikiri::simcore
