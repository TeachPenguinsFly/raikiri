#include <raikiri/venues/kraken/book.hpp>
#include <raikiri/venues/kraken/envelope.hpp>
#include <raikiri/venues/kraken/map.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kraken {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry) {
    wire::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    const auto price = req_num(obj, "price");
    const auto qty = req_num(obj, "qty");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
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
    return count;
}

// The venue's own reader: parse_decimal_e9 refuses the exponents this venue
// writes. The exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_number_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto feed = wire::req_str(*root, "feed");
        const auto product = wire::req_str(*root, "product_id");
        if (!feed.has_value() || !product.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireBook out{};
        out.product_id = *product;
        out.snapshot = *feed == "book_snapshot";
        // `seq` is the continuity chain and runs contiguous across both.
        const auto timestamp = opt_ms(*root, "timestamp");
        const auto seq = opt_num(*root, "seq");
        if (!timestamp.has_value() || !seq.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.timestamp_ms = *timestamp;
        out.seq = wire::digits_to_u64(*seq);

        if (out.snapshot) {
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
        }
        const auto side = wire::req_str(*root, "side");
        const auto price = req_num(*root, "price");
        const auto qty = req_num(*root, "qty");
        if (!side.has_value() || !price.has_value() || !qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const WireLevel level{.price = *price, .qty = *qty};
        if (*side == "buy") {
            storage.bids[0] = level;
            out.bids = {storage.bids.data(), 1};
        } else if (*side == "sell") {
            storage.asks[0] = level;
            out.asks = {storage.asks.data(), 1};
        } else {
            return std::unexpected(WireError::WrongType);
        }
        return out;
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
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

        if (level.qty.raw == 0) {
            // Quantity zero is Kraken's delete: 615 of 8069 live deltas.
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) ||
            !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_seq_ = update.seq;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot has no book to chain onto.
        return BookResult::Ignored;
    }
    if (update.seq != last_seq_ + 1) {
        if (update.seq <= last_seq_) {
            return BookResult::Ignored; // redelivery of a frame already applied
        }
        ++gaps_;
        invalidate();
        return BookResult::GapDetected;
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

} // namespace raikiri::venues::kraken
