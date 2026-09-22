#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/okx/book.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;
using wire::session;

// A level is ["64043.6","1282.19","0","57"]: price, size, a deprecated field
// pinned to "0", and the order count. Only the first two are read.
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
            (field == 0 ? level.price : level.qty) = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::MissingField);
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

// Top of book only. An empty side is a real state on an illiquid contract, so
// it yields empty views rather than an error; the mapper drops those.
std::expected<void, WireError> read_top(wire::ondemand::object& obj,
                                        std::string_view key,
                                        std::string_view& price,
                                        std::string_view& qty) {
    auto array = wire::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value);
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        price = level->price;
        qty = level->qty;
        break;
    }
    return {};
}

} // namespace

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        return with_first_data(
            *root, [&](wire::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
                WireBbo out{};
                out.inst_id = *inst_id;
                if (const auto ok = read_top(obj, "bids", out.bid_price, out.bid_qty);
                    !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                if (const auto ok = read_top(obj, "asks", out.ask_price, out.ask_qty);
                    !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                const auto ts = wire::opt_ms(obj, "ts");
                const auto seq = wire::opt_i64(obj, "seqId");
                if (!ts.has_value() || !seq.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ms = *ts;
                out.seq_id = *seq;
                return out;
            });
    });
}

std::expected<WireBooks, WireError> parse_books(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBooks, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        // `action` sits beside `arg`, not inside the data element.
        const auto action = wire::opt_str(*root, "action");
        if (!action.has_value()) {
            return std::unexpected(action.error());
        }
        const bool snapshot = *action == "snapshot";
        return with_first_data(
            *root, [&](wire::ondemand::object& obj) -> std::expected<WireBooks, WireError> {
                WireBooks out{};
                out.inst_id = *inst_id;
                out.snapshot = snapshot;
                const auto bids = read_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = wire::opt_ms(obj, "ts");
                const auto seq = wire::opt_i64(obj, "seqId");
                const auto prev = wire::opt_i64(obj, "prevSeqId");
                if (!ts.has_value() || !seq.has_value() || !prev.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ms = *ts;
                out.seq_id = *seq;
                out.prev_seq_id = *prev;
                out.bids = {storage.bids.data(), *bids};
                out.asks = {storage.asks.data(), *asks};
                return out;
            });
    });
}

namespace {

// The venue's own reader; the exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
    broken_ = false;
    last_seq_id_ = -1;
}

bool Book::resync_due(std::uint64_t now_ns) noexcept {
    if (!broken_ || (resync_asked_ns_ != 0 && now_ns - resync_asked_ns_ < k_resync_backoff_ns)) {
        return false;
    }
    resync_asked_ns_ = now_ns;
    return true;
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
            // Quantity zero is OKX's delete.
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
    return true;
}

BookResult Book::apply(const WireBooks& update) {
    if (update.snapshot) {
        ladders_.clear();
        if (!apply_side(update.bids, ladders_.bids()) ||
            !apply_side(update.asks, ladders_.asks())) {
            invalidate();
            broken_ = true;
            return BookResult::BadLevel;
        }
        last_seq_id_ = update.seq_id;
        synced_ = true;
        broken_ = false;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before any snapshot has no book to chain onto.
        return BookResult::Ignored;
    }
    if (update.prev_seq_id != last_seq_id_) {
        if (update.seq_id <= last_seq_id_) {
            return BookResult::Ignored; // redelivery of a frame already applied
        }
        ++gaps_;
        invalidate();
        broken_ = true;
        return BookResult::GapDetected;
    }

    if (!apply_side(update.bids, ladders_.bids()) || !apply_side(update.asks, ladders_.asks())) {
        invalidate();
        broken_ = true;
        return BookResult::BadLevel;
    }
    last_seq_id_ = update.seq_id;
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    return ladders_.snapshot(
        instrument_, static_cast<std::uint64_t>(last_seq_id_), ts_event, ts_init);
}

} // namespace raikiri::venues::okx
