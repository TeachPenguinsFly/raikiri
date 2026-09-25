#include <raikiri/venues/blofin/book.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::blofin {

namespace {

using wire::guarded;
using wire::session;

std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry) {
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
        if (field < 2) {
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            (field == 0 ? level.price : level.size) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::ShortArray);
    }
    return level;
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
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
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

// seqId is a digit string; an absent or unreadable one cannot be chained
std::expected<std::uint64_t, WireError> sequence(wire::ondemand::object& obj, std::string_view key) {
    const auto text = wire::req_str(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    std::uint64_t out = 0;
    const auto* const end = text->data() + text->size();
    const auto [stop, ec] = std::from_chars(text->data(), end, out);
    if (ec != std::errc{} || stop != end) {
        return std::unexpected(WireError::WrongType);
    }
    return out;
}

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireBooks, WireError> parse_books(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBooks, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto arg = wire::req_object(*root, "arg");
        if (!arg.has_value()) {
            return std::unexpected(arg.error());
        }
        const auto inst_id = wire::req_str(*arg, "instId");
        const auto action = wire::req_str(*root, "action");
        if (!inst_id.has_value() || !action.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireBooks out{};
        out.inst_id = *inst_id;
        out.snapshot = *action == "snapshot";
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto ts = wire::opt_ms(*data, "ts");
        const auto prev = sequence(*data, "prevSeqId");
        const auto seq = sequence(*data, "seqId");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        if (!prev.has_value() || !seq.has_value()) {
            return std::unexpected(prev.has_value() ? seq.error() : prev.error());
        }
        out.ts_ms = *ts;
        out.prev_seq_id = *prev;
        out.seq_id = *seq;
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
    seq_id_ = 0;
}

bool Book::apply_side(std::span<const WireLevel> levels, book::Ladder& side) const {
    for (const auto& wire : levels) {
        const auto price = steps_of(wire.price, tick_size_);
        const auto qty = steps_of(wire.size, lot_size_);
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

BookResult Book::apply(const WireBooks& frame) {
    if (frame.snapshot) {
        ladders_.clear();
        if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        seq_id_ = frame.seq_id;
        synced_ = true;
        return BookResult::Resynced;
    }
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (frame.prev_seq_id != seq_id_) {
        if (frame.seq_id <= seq_id_) {
            return BookResult::Ignored;
        }
        ++gaps_;
        invalidate();
        return BookResult::Gap;
    }
    if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
        invalidate();
        return BookResult::BadLevel;
    }
    seq_id_ = frame.seq_id;
    return BookResult::Applied;
}

std::optional<Bbo> Book::top(UnixNanos ts_event, UnixNanos ts_init) const {
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

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(instrument_, seq_id_, ts_event, ts_init);
}

} // namespace raikiri::venues::blofin
