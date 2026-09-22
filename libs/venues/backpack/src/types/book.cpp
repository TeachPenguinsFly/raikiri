#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/backpack/book.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::backpack {

namespace {

using wire::guarded;
using wire::session;

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
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

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::optional<std::int64_t> tradable_steps(std::string_view text, FixedE9 step) noexcept {
    const auto value = parse_decimal_e9(text);
    if (!value.has_value() || value->raw < 0 || step.raw <= 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

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
        // All four required: an empty side published as zero is a price nobody offered.
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
        const auto engine = wire::opt_u64(*root, "T");
        if (!update_id.has_value() || !engine.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.engine_us = *engine;
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
        const auto engine = wire::opt_u64(*root, "T");
        if (!first.has_value() || !last.has_value() || !engine.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*first == 0 || *last < *first) {
            return std::unexpected(WireError::MissingField);
        }
        out.first_id = *first;
        out.last_id = *last;
        out.engine_us = *engine;
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
        const auto last = wire::opt_str(*root, "lastUpdateId");
        const auto stamp = wire::opt_u64(*root, "timestamp");
        if (!last.has_value() || !stamp.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.last_update_id = wire::digits_to_u64(*last);
        if (out.last_update_id == 0) {
            return std::unexpected(WireError::MissingField);
        }
        out.timestamp_us = *stamp;
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
    const auto qty = tradable_steps(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void DepthBook::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

bool DepthBook::apply_wire(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto level = convert(wire);
        if (!level.has_value()) {
            return false;
        }
        upsert(side, *level);
    }
    return true;
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
    case State::Live:
        return advance(delta);
    }
    return BookResult::Stale;
}

BookResult DepthBook::buffer(const WireDepthUpdate& delta) {
    if (!pending_.empty() && delta.first_id != pending_.back().last_id + 1) {
        pending_.clear();
    }
    if (pending_.size() == k_max_buffered_deltas) {
        pending_.clear();
    }
    Buffered held{.first_id = delta.first_id, .last_id = delta.last_id};
    const auto take = [this](std::span<const WireLevel> src, std::vector<Level>& into) {
        into.reserve(src.size());
        for (const auto& wire : src) {
            const auto level = convert(wire);
            if (!level.has_value()) {
                return false;
            }
            into.push_back(*level);
        }
        return true;
    };
    if (!take(delta.bids, held.bids) || !take(delta.asks, held.asks)) {
        pending_.clear();
        return BookResult::Buffered;
    }
    pending_.push_back(std::move(held));
    return BookResult::Buffered;
}

// A delta straddling our id is applied whole: its levels are absolute sizes as of its `u`.
BookResult DepthBook::advance(const WireDepthUpdate& delta) {
    if (delta.last_id <= last_id_) {
        return BookResult::Stale;
    }
    if (delta.first_id > last_id_ + 1) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    return step(delta);
}

BookResult DepthBook::step(const WireDepthUpdate& delta) {
    // An off-grid level drops the whole book; skipping it would diverge silently.
    if (!apply_wire(delta.bids, ladders_.bids()) || !apply_wire(delta.asks, ladders_.asks())) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    last_id_ = delta.last_id;
    return settle();
}

BookResult DepthBook::seed(const WireDepthSnapshot& wire) {
    seed_in_flight_ = false;
    // A newer seed replaces a live book, so two racing seeds land oldest first.
    if (state_ == State::Live && wire.last_update_id <= last_id_) {
        return BookResult::Stale;
    }
    held_.clear();
    const bool exact = apply_wire(wire.bids, held_.bids()) && apply_wire(wire.asks, held_.asks());
    if (!exact || held_.bids().empty() || held_.asks().empty()) {
        discard_held();
        return BookResult::Reseed;
    }
    held_id_ = wire.last_update_id;
    band_low_ = held_.bids().levels().back().price;
    band_high_ = held_.asks().levels().back().price;
    return replay();
}

// Live as soon as the seed lands: ids are dense, and waiting for a delta to vouch
// for the seed left quiet books cold for a minute.
BookResult DepthBook::replay() {
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].last_id <= held_id_) {
        ++i;
    }
    if (i < pending_.size() && pending_[i].first_id > held_id_ + 1) {
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    for (; i < pending_.size(); ++i) {
        apply_levels(pending_[i].bids, ladders_.bids());
        apply_levels(pending_[i].asks, ladders_.asks());
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    // Levels past the seed's band were never confirmed by a snapshot.
    ladders_.bids().drop_beyond(band_low_);
    ladders_.asks().drop_beyond(band_high_);
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
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

} // namespace raikiri::venues::backpack
