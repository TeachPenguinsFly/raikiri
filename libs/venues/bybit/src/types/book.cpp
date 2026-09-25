#include <raikiri/venues/bybit/book.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bybit {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

std::expected<std::size_t, WireError> read_levels(ondemand::array levels, WireLevel* out, std::size_t capacity) {
    std::size_t count = 0;
    for (auto entry : levels) {
        if (count == capacity) {
            return std::unexpected{WireError::TooManyEntries};
        }
        ondemand::array pair;
        if (entry.get_array().get(pair) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        std::size_t field = 0;
        WireLevel level{};
        for (auto item : pair) {
            ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected{text.error()};
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.size = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected{WireError::MissingField};
        }
        out[count++] = level;
    }
    return count;
}

} // namespace

std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame, OrderbookBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderbook, WireError> {
        auto& session = wire::session();
        auto root = session.open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;

        WireOrderbook out{};
        const auto type = wire::opt_str(obj, "type");
        out.is_snapshot = type.value_or(std::string_view{}) == "snapshot";
        obj.reset();
        out.ts_ms = wire::opt_u64(obj, "ts").value_or(0);

        obj.reset();
        auto data = wire::req_object(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }
        auto data_obj = *data;

        auto symbol = wire::req_str(data_obj, "s");
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        out.symbol = *symbol;

        data_obj.reset();
        out.update_id = wire::opt_u64(data_obj, "u").value_or(0);
        data_obj.reset();
        out.seq = wire::opt_u64(data_obj, "seq").value_or(0);

        data_obj.reset();
        std::size_t bid_count = 0;
        if (auto bids = wire::req_array(data_obj, "b"); bids.has_value()) {
            const auto n = read_levels(*bids, storage.bids, k_max_levels);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            bid_count = *n;
        }
        data_obj.reset();
        std::size_t ask_count = 0;
        if (auto asks = wire::req_array(data_obj, "a"); asks.has_value()) {
            const auto n = read_levels(*asks, storage.asks, k_max_levels);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            ask_count = *n;
        }
        out.bids = {storage.bids, bid_count};
        out.asks = {storage.asks, ask_count};
        return out;
    });
}

namespace {

// the exact-or-refuse rule is book::exact_steps
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

BookSync::BookSync(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void BookSync::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_update_id_ = 0;
    last_seq_ = 0;
}

bool BookSync::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto size = steps_of(wire.size, lot_size_);
        if (!price.has_value() || !size.has_value()) {
            return false;
        }
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*size)};

        if (level.qty.raw == 0) {
            // size zero deletes the level
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

SyncResult BookSync::apply(const WireOrderbook& update) {
    if (update.is_snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return SyncResult::BadLevel;
        }
        last_update_id_ = update.update_id;
        last_seq_ = update.seq;
        synced_ = true;
        return SyncResult::Resynced;
    }

    if (!synced_) {
        return SyncResult::Ignored;
    }
    if (update.update_id <= last_update_id_) {
        return SyncResult::Ignored; // duplicate or reordered; already applied
    }
    if (update.update_id != last_update_id_ + 1) {
        ++gaps_;
        invalidate();
        return SyncResult::GapDetected;
    }

    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        return SyncResult::BadLevel;
    }
    last_update_id_ = update.update_id;
    last_seq_ = update.seq;
    return SyncResult::Applied;
}

std::optional<Bbo> BookSync::bbo(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.bid_qty = bid.qty;
    out.ask = ask.price;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

std::optional<BookSnapshot> BookSync::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, last_update_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::bybit
