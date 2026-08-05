#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/gate/book.hpp>
#include <raikiri/venues/gate/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <utility>

namespace raikiri::venues::gate {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// A level is {"p":"64156.4","s":35680} on the websocket and in the REST
// snapshot alike — same object either way, unlike okx's positional arrays.
std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object level;
        if (const auto ec = entry.get_object().get(level); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto price = req_num(level, "p");
        const auto qty = req_num(level, "s");
        if (!price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = *price, .qty = *qty};
        ++count;
    }
    return count;
}

// The spot book spells a level as a two-element ARRAY, ["79520","0.054448"],
// where the futures deltas spell it as an object {"p":...,"s":...}. Same
// numbers, same order, no shared reader.
std::expected<std::size_t, WireError>
read_pair_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        std::string_view fields[2];
        std::size_t seen = 0;
        for (auto field : pair) {
            if (seen == 2) {
                return std::unexpected(WireError::WrongType);
            }
            const auto text = num_of(field.value());
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            fields[seen++] = *text;
        }
        if (seen != 2) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = fields[0], .qty = fields[1]};
        ++count;
    }
    return count;
}

// The venue's own reader; the exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_result_object(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireBookTicker, WireError> {
                WireBookTicker out{};
                const auto ts = num_u64(obj, "t");
                const auto update_id = num_u64(obj, "u");
                const auto contract = detail::req_str(obj, "s");
                const auto bid_price = req_num(obj, "b");
                const auto bid_qty = req_num(obj, "B");
                const auto ask_price = req_num(obj, "a");
                const auto ask_qty = req_num(obj, "A");
                if (!ts.has_value() || !update_id.has_value() || !contract.has_value() ||
                    !bid_price.has_value() || !bid_qty.has_value() || !ask_price.has_value() ||
                    !ask_qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                out.update_id = *update_id;
                out.contract = *contract;
                out.bid_price = *bid_price;
                out.bid_qty = *bid_qty;
                out.ask_price = *ask_price;
                out.ask_qty = *ask_qty;
                return out;
            });
    });
}

std::expected<std::span<const WireBookTicker>, WireError>
parse_futures_tickers(std::string_view body, TickerQuoteBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireBookTicker>, WireError> {
        auto root = session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::size_t count = 0;
        for (auto entry : *root) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            const auto contract = detail::req_str(obj, "contract");
            if (!contract.has_value()) {
                return std::unexpected(contract.error());
            }
            WireBookTicker out{};
            out.contract = *contract;
            out.bid_price = field(obj, "highest_bid");
            out.bid_qty = field(obj, "highest_size");
            out.ask_price = field(obj, "lowest_ask");
            out.ask_qty = field(obj, "lowest_size");
            storage.rows[count++] = out;
        }
        return std::span<const WireBookTicker>{storage.rows.data(), count};
    });
}

std::expected<WireBookUpdate, WireError> parse_book_update(std::string_view frame,
                                                           LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBookUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_result_object(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBookUpdate, WireError> {
                WireBookUpdate out{};
                const auto ts = num_u64(obj, "t");
                const auto first = num_u64(obj, "U");
                const auto last = num_u64(obj, "u");
                const auto contract = detail::req_str(obj, "s");
                if (!ts.has_value() || !first.has_value() || !last.has_value() ||
                    !contract.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                out.first_update_id = *first;
                out.last_update_id = *last;
                out.contract = *contract;
                const auto asks = read_levels(obj, "a", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = read_levels(obj, "b", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                out.asks = {storage.asks.data(), *asks};
                out.bids = {storage.bids.data(), *bids};
                return out;
            });
    });
}

std::expected<WireBookUpdate, WireError> parse_book_snapshot(std::string_view frame,
                                                             LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBookUpdate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_result_object(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBookUpdate, WireError> {
                WireBookUpdate out{};
                const auto ts = num_u64(obj, "t");
                const auto last = num_u64(obj, "lastUpdateId");
                const auto contract = detail::req_str(obj, "s");
                if (!ts.has_value() || !last.has_value() || !contract.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.ts_ms = *ts;
                // Both ids are the same number on purpose: a snapshot IS its
                // own first and last update, and there is no chain to join.
                out.first_update_id = *last;
                out.last_update_id = *last;
                out.contract = *contract;
                const auto asks = read_pair_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto bids = read_pair_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                out.asks = {storage.asks.data(), *asks};
                out.bids = {storage.bids.data(), *bids};
                return out;
            });
    });
}

std::expected<WireOrderBook, WireError> parse_order_book(std::string_view body,
                                                         LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderBook, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrderBook out{};
        const auto id = num_u64(*root, "id");
        const auto update = num_u64(*root, "update");
        if (!id.has_value() || !update.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (*id == 0) {
            // The request went out without with_id=true. Levels with no place
            // in the update chain cannot seed anything.
            return std::unexpected(WireError::MissingField);
        }
        out.update_id = *id;
        out.update_s = *update;
        const auto asks = read_levels(*root, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto bids = read_levels(*root, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        out.asks = {storage.asks.data(), *asks};
        out.bids = {storage.bids.data(), *bids};
        return out;
    });
}

DepthBook::DepthBook(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

bool DepthBook::wants_seed() const noexcept {
    return state_ == State::Cold && !seed_in_flight_;
}

void DepthBook::invalidate() noexcept {
    ladders_.clear();
    pending_.clear();
    held_.clear();
    held_id_ = 0;
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
        // Size zero is Gate's delete, and it is also how a level leaving the
        // subscribed window is retired — half the frames carry nothing else.
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

BookResult DepthBook::apply(const WireBookUpdate& delta) {
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

BookResult DepthBook::buffer(const WireBookUpdate& delta) {
    if (!pending_.empty() && delta.first_update_id != pending_.back().last_id + 1) {
        pending_.clear(); // the chain broke before the seed landed
    }
    if (pending_.size() == k_max_buffered_deltas) {
        pending_.clear();
    }
    Buffered held{.first_id = delta.first_update_id, .last_id = delta.last_update_id};
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

BookResult DepthBook::bridge(const WireBookUpdate& delta) {
    const auto wanted = held_id_ + 1;
    if (delta.last_update_id < wanted) {
        return BookResult::Stale; // every id here is already in the seed
    }
    if (delta.first_update_id > wanted) {
        // The stream has run past the seed and the chain is contiguous, so
        // nothing will ever fill what is now missing between them.
        discard_held();
        return BookResult::Reseed;
    }
    install_held();
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_update_id;
    return settle();
}

BookResult DepthBook::advance(const WireBookUpdate& delta) {
    if (delta.last_update_id <= last_id_) {
        return BookResult::Stale; // duplicate or reordered; already applied
    }
    if (delta.first_update_id != last_id_ + 1) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    apply_wire(delta.bids, ladders_.bids());
    apply_wire(delta.asks, ladders_.asks());
    last_id_ = delta.last_update_id;
    return settle();
}

BookResult DepthBook::seed(const WireOrderBook& wire) {
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
    held_id_ = wire.update_id;
    state_ = State::Bridging;
    return replay();
}

BookResult DepthBook::replay() {
    const auto wanted = held_id_ + 1;
    std::size_t i = 0;
    while (i < pending_.size() && pending_[i].last_id < wanted) {
        ++i;
    }
    if (i == pending_.size()) {
        pending_.clear();
        return BookResult::Waiting; // the seed is newer than anything buffered
    }
    if (pending_[i].first_id > wanted) {
        discard_held();
        return BookResult::Reseed;
    }

    install_held();
    // buffer() already enforced contiguity across pending_, so the replay only
    // has to find where the seed sits inside it.
    for (; i < pending_.size(); ++i) {
        apply_levels(pending_[i].bids, ladders_.bids());
        apply_levels(pending_[i].asks, ladders_.asks());
        last_id_ = pending_[i].last_id;
    }
    pending_.clear();
    return settle();
}

BookResult DepthBook::settle() {
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // Gate maintains the window, so a side emptying means what is held no
        // longer describes the venue's book; a fresh read is the way back.
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
    state_ = State::Cold;
    ++reseeds_;
}

std::optional<BookSnapshot> DepthBook::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (state_ != State::Live) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::gate
