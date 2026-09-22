#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/aster/book.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::aster {

namespace {

using wire::guarded;
using wire::session;

// A level is a two-element array of strings, ["64488.50","5.560"], on the
// depth stream and in the REST snapshot alike.
std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        WireLevel level{};
        std::size_t field = 0;
        for (auto element : pair) {
            wire::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

// The venue's own reader: aster writes plain decimals. The exact-or-refuse
// rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view data) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBookTicker out{};
        const auto symbol = wire::req_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        for (const auto& [key, field] : {std::pair{"b", &out.bid_price},
                                         std::pair{"B", &out.bid_qty},
                                         std::pair{"a", &out.ask_price},
                                         std::pair{"A", &out.ask_qty}}) {
            const auto value = wire::req_str(*root, key);
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            *field = *value;
        }
        const auto update_id = wire::opt_u64(*root, "u");
        const auto transaction = wire::opt_u64(*root, "T");
        const auto event = wire::opt_u64(*root, "E");
        if (!update_id.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.transaction_ms = *transaction;
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireDepthUpdate, WireError> parse_depth_update(std::string_view data,
                                                             LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthUpdate, WireError> {
        auto root = session().open_object(data);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireDepthUpdate out{};
        const auto symbol = wire::req_str(*root, "s");
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        out.symbol = *symbol;
        const auto first = wire::opt_u64(*root, "U");
        const auto last = wire::opt_u64(*root, "u");
        const auto prev = wire::opt_u64(*root, "pu");
        const auto event = wire::opt_u64(*root, "E");
        if (!first.has_value() || !last.has_value() || !prev.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.first_id = *first;
        out.last_id = *last;
        out.prev_id = *prev;
        out.event_ms = *event;

        const auto bids = read_levels(*root, "b", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*root, "a", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireDepthSnapshot, WireError> parse_depth_snapshot(std::string_view body,
                                                                 LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireDepthSnapshot, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireDepthSnapshot out{};
        const auto last = wire::opt_u64(*root, "lastUpdateId");
        const auto transaction = wire::opt_u64(*root, "T");
        const auto event = wire::opt_u64(*root, "E");
        if (!last.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*last == 0) {
            return std::unexpected(WireError::MissingField); // a seed with no id seeds nothing
        }
        out.last_update_id = *last;
        out.transaction_ms = *transaction;
        out.event_ms = *event;

        const auto bids = read_levels(*root, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*root, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {
    ladders_.bids().reserve(k_seed_depth);
    ladders_.asks().reserve(k_seed_depth);
}

bool DepthBook::wants_seed() const noexcept {
    return state_ == State::Cold && !seed_in_flight_;
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    pending_.clear();
    held_.clear();
    held_id_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    last_id_ = 0;
    state_ = State::Cold;
    seed_in_flight_ = false;
}

std::optional<DepthBook::Level> DepthBook::convert(const WireLevel& wire) const {
    const auto price = steps_of(wire.price, tick_size_);
    const auto qty = steps_of(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is aster's delete, on the seed and on the deltas alike.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

void DepthBook::apply_wire(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto level = convert(wire);
        if (!level.has_value()) {
            ++dropped_levels_;
            continue;
        }
        upsert(side, *level);
    }
}

void DepthBook::apply_levels(std::span<const Level> levels, book::Ladder& side) {
    for (const auto& level : levels) {
        upsert(side, level);
    }
}

BookResult DepthBook::apply(const WireDepthUpdate& delta) {
    switch (state_) {
    case State::Cold:
        return buffer(delta);
    case State::Bridging:
        return bridge(delta);
    case State::Live:
        return advance(delta);
    }
    return BookResult::Waiting;
}

BookResult DepthBook::buffer(const WireDepthUpdate& delta) {
    if (!pending_.empty() && delta.prev_id != pending_.back().last_id) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_deltas) {
        pending_.clear();
    }
    Buffered held{.first_id = delta.first_id, .last_id = delta.last_id, .prev_id = delta.prev_id};
    const auto take = [this](std::span<const WireLevel> src, std::vector<Level>& into) {
        into.reserve(src.size());
        for (const auto& wire : src) {
            const auto level = convert(wire);
            if (!level.has_value()) {
                ++dropped_levels_;
                continue;
            }
            into.push_back(*level);
        }
    };
    take(delta.bids, held.bids);
    take(delta.asks, held.asks);
    pending_.push_back(std::move(held));
    return BookResult::Buffered;
}

BookResult DepthBook::bridge(const WireDepthUpdate& delta) {
    if (delta.last_id < held_id_) {
        return BookResult::Stale;
    }
    if (delta.first_id > held_id_) {
        // The stream has moved past the photograph, and no delta will ever
        // carry the one into the other.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_id;
    return settle();
}

BookResult DepthBook::advance(const WireDepthUpdate& delta) {
    if (delta.last_id <= last_id_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (delta.prev_id != last_id_) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_id;
    return settle();
}

BookResult DepthBook::seed(const WireDepthSnapshot& wire) {
    seed_in_flight_ = false;
    if (state_ == State::Live) {
        return BookResult::Stale; // a seed outlived the book it was meant for
    }
    held_.clear();
    apply_wire(wire.bids, held_.bids());
    apply_wire(wire.asks, held_.asks());
    if (held_.bids().empty() || held_.asks().empty()) {
        discard_held();
        return BookResult::Reseed;
    }
    held_id_ = wire.last_update_id;
    band_low_ = held_.bids().levels().back().price;
    band_high_ = held_.asks().levels().back().price;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].last_id < held_id_) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].first_id > held_id_) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced the pu chain across pending_, so the replay
    // only has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        apply_levels(pending_[i].bids, ladders_.bids());
        apply_levels(pending_[i].asks, ladders_.asks());
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    // Past the band the seed vouched for, a level is one no snapshot ever
    // confirmed; carrying it would make the book look deeper than it is known
    // to be.
    ladders_.bids().drop_beyond(band_low_);
    ladders_.asks().drop_beyond(band_high_);
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
        invalidate();
        return BookResult::Reseed;
    }
    return BookResult::Applied;
}

void DepthBook::install_held() {
    ladders_ = std::move(held_);
    held_.clear();
    last_id_ = held_id_;
    state_ = State::Live;
}

void DepthBook::discard_held() {
    held_.clear();
    held_id_ = 0;
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::aster
