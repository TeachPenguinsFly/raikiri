#include <raikiri/venues/deribit/book.hpp>
#include <raikiri/venues/deribit/envelope.hpp>
#include <raikiri/venues/deribit/map.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::deribit {

namespace {

using wire::guarded;
using wire::session;

LevelAction action_from(std::string_view text) noexcept {
    if (text == "delete") {
        return LevelAction::Delete;
    }
    return text == "change" ? LevelAction::Change : LevelAction::New;
}

// grouped books ship [price, amount], the incremental feed [action, price, amount]
std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry, bool expect_action) {
    wire::ondemand::array tuple;
    if (const auto ec = entry.get_array().get(tuple); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    WireLevel level{};
    const std::size_t want = expect_action ? 3U : 2U;
    std::size_t field = 0;
    for (auto element : tuple) {
        wire::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        if (field >= want) {
            break;
        }
        if (expect_action && field == 0) {
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            level.action = action_from(*text);
        } else {
            const auto token = num_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            (field == want - 2 ? level.price : level.qty) = *token;
        }
        ++field;
    }
    if (field < want) {
        return std::unexpected(WireError::MissingField);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& obj, std::string_view key, bool expect_action, std::span<WireLevel> into) {
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value, expect_action);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

// an empty side is real on an illiquid option
std::expected<void, WireError>
read_top(wire::ondemand::object& obj, std::string_view key, std::string_view& price, std::string_view& qty) {
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value, /*expect_action=*/false);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        price = level->price;
        qty = level->qty;
        break;
    }
    return {};
}

// sizes also arrive as 6.6e3, which book::exact_steps cannot read; a remainder is refused
std::optional<std::int64_t> exact_steps(std::string_view token, FixedE9 step) {
    if (token.empty() || step.raw <= 0) {
        return std::nullopt;
    }
    const auto value = parse_number_e9(token);
    if (!value.has_value() || value->raw % step.raw != 0) {
        return std::nullopt;
    }
    return value->raw / step.raw;
}

} // namespace

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBbo out{};
        const auto instrument = wire::req_str(*data, "instrument_name");
        if (!instrument.has_value()) {
            return std::unexpected(instrument.error());
        }
        out.instrument = *instrument;
        if (const auto ok = read_top(*data, "bids", out.bid_price, out.bid_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        if (const auto ok = read_top(*data, "asks", out.ask_price, out.ask_qty); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        const auto ts = opt_ms(*data, "timestamp");
        const auto change = wire::opt_i64(*data, "change_id");
        if (!ts.has_value() || !change.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.change_id = *change;
        return out;
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBook out{};
        const auto instrument = wire::req_str(*data, "instrument_name");
        const auto type = wire::req_str(*data, "type");
        if (!instrument.has_value() || !type.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.instrument = *instrument;
        out.snapshot = *type == "snapshot";
        const auto bids = read_levels(*data, "bids", /*expect_action=*/true, storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", /*expect_action=*/true, storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto ts = opt_ms(*data, "timestamp");
        const auto change = wire::opt_i64(*data, "change_id");
        // absent on a snapshot
        const auto prev = wire::opt_i64(*data, "prev_change_id");
        if (!ts.has_value() || !change.has_value() || !prev.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.change_id = *change;
        out.prev_change_id = *prev;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    last_change_id_ = -1;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) {
    for (const auto& wire : levels) {
        const auto price = exact_steps(wire.price, tick_size_);
        const auto qty = exact_steps(wire.qty, lot_size_);
        if (!price.has_value() || !qty.has_value()) {
            return false;
        }
        const book::Level level{PriceTicks::from_raw(*price), QtyLots::from_raw(*qty)};

        // delete is deribit's removal; a new or change of zero is no level either
        if (wire.action == LevelAction::Delete || level.qty.raw == 0) {
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
        if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        last_change_id_ = update.change_id;
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        return BookResult::Ignored;
    }
    if (update.prev_change_id != last_change_id_) {
        if (update.change_id <= last_change_id_) {
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
    last_change_id_ = update.change_id;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, static_cast<std::uint64_t>(last_change_id_), ts_event, ts_init);
}

} // namespace raikiri::venues::deribit
