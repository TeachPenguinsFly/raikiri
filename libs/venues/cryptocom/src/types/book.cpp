#include <raikiri/venues/cryptocom/book.hpp>
#include <raikiri/wire/json.hpp>

#include "fields.hpp"

namespace raikiri::venues::cryptocom {

namespace {

std::expected<WireLevel, WireError> read_level(ondemand::value& entry) {
    ondemand::array triple;
    if (const auto ec = entry.get_array().get(triple); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : triple) {
        ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (field < 2) {
            if (const auto read = text_into(value, field == 0 ? level.price : level.qty); !read.has_value()) {
                return std::unexpected(read.error());
            }
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

FieldResult read_levels(ondemand::value& side, std::span<WireLevel> into, std::size_t& count) {
    ondemand::array levels;
    if (const auto ec = side.get_array().get(levels); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    count = 0;
    for (auto entry : levels) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return {};
}

struct Sides {
    std::size_t asks{0};
    std::size_t bids{0};
    bool seen{false};
};

FieldResult read_sides(std::string_view key, ondemand::value& value, LevelBuffer& storage, Sides& sides) {
    if (key == "asks") {
        sides.seen = true;
        return read_levels(value, storage.asks, sides.asks);
    }
    if (key == "bids") {
        sides.seen = true;
        return read_levels(value, storage.bids, sides.bids);
    }
    return {};
}

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBook, WireError> parse_book(std::string_view data, bool snapshot, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireBook, WireError> {
        auto array = wire::session().open_array(data);
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        for (auto entry : *array) {
            ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireBook out{};
            out.snapshot = snapshot;
            Sides sides{};
            const auto read = each_field(row, [&](std::string_view key, ondemand::value& value) -> FieldResult {
                if (key == "t") {
                    return u64_into(value, out.ts_ms);
                }
                if (key == "u") {
                    return u64_into(value, out.seq);
                }
                if (key == "pu") {
                    return u64_into(value, out.prev_seq);
                }
                if (snapshot) {
                    return read_sides(key, value, storage, sides);
                }
                if (key != "update") {
                    return {};
                }
                ondemand::object update;
                if (const auto ec = value.get_object().get(update); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                return each_field(update, [&](std::string_view side, ondemand::value& levels) {
                    return read_sides(side, levels, storage, sides);
                });
            });
            if (!read.has_value()) {
                return std::unexpected(read.error());
            }
            if (!sides.seen || out.seq == 0 || (!snapshot && out.prev_seq == 0)) {
                return std::unexpected(WireError::MissingField);
            }
            out.asks = {storage.asks.data(), sides.asks};
            out.bids = {storage.bids.data(), sides.bids};
            return out;
        }
        return std::unexpected(WireError::MissingField);
    });
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_seq_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto qty = steps_of(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        if (*qty == 0) {
            side.remove(PriceTicks::from_raw(*price));
            continue;
        }
        side.upsert({PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)});
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_seq_ = update.seq;
        synced_ = true;
        return BookResult::Resynced;
    }
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (update.prev_seq != last_seq_) {
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    last_seq_ = update.seq;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_seq_, ts_event, ts_init);
}

std::optional<Bbo> Book::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    const auto touch = ladders_.touch();
    if (!touch.has_value()) {
        return std::nullopt;
    }
    Bbo out{};
    out.instrument = instrument_;
    out.bid = touch->first.price;
    out.bid_qty = touch->first.qty;
    out.ask = touch->second.price;
    out.ask_qty = touch->second.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::cryptocom
