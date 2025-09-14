#include <raikiri/common/fixed_point.hpp>
#include <raikiri/venues/coinbase/book.hpp>

namespace raikiri::venues::coinbase {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// ["price","size"], both decimal strings. Reading stops at the buffer rather
// than failing: see the header on why truncating this particular array is safe.
std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            break;
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        std::size_t field = 0;
        for (auto element : pair) {
            detail::ondemand::value value;
            if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            // str_of, never get_string: the unescaping reader writes into a
            // buffer that cannot be rebased onto the caller's frame, and what
            // comes back from that is an empty view rather than a price.
            const auto text = detail::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            if (field == 0) {
                into[count].price = *text;
            } else if (field == 1) {
                into[count].qty = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected(WireError::ShortArray);
        }
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

std::expected<WireL2Snapshot, WireError> parse_l2_snapshot(std::string_view frame,
                                                           LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireL2Snapshot, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        WireL2Snapshot out{};
        out.product_id = *product;
        const auto bids = read_levels(*root, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*root, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        const auto time = detail::opt_str(*root, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        out.time = *time;
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireL2Update, WireError> parse_l2_update(std::string_view frame,
                                                       ChangeBuffer& storage) {
    return guarded([&]() -> std::expected<WireL2Update, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto product = detail::req_str(*root, "product_id");
        if (!product.has_value()) {
            return std::unexpected(product.error());
        }
        WireL2Update out{};
        out.product_id = *product;
        auto array = detail::req_array(*root, "changes");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_changes) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::array triple;
            if (const auto ec = entry.get_array().get(triple); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireChange change{};
            std::size_t field = 0;
            for (auto element : triple) {
                detail::ondemand::value value;
                if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                const auto text = detail::str_of(value);
                if (!text.has_value()) {
                    return std::unexpected(text.error());
                }
                if (field == 0) {
                    change.bid = *text == "buy";
                } else if (field == 1) {
                    change.price = *text;
                } else if (field == 2) {
                    change.qty = *text;
                }
                ++field;
            }
            if (field < 3) {
                return std::unexpected(WireError::ShortArray);
            }
            storage.changes[count] = change;
            ++count;
        }
        const auto time = detail::opt_str(*root, "time");
        if (!time.has_value()) {
            return std::unexpected(time.error());
        }
        out.time = *time;
        out.changes = {storage.changes.data(), count};
        return out;
    });
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    band_low_ = PriceTicks{};
    band_high_ = PriceTicks{};
    synced_ = false;
}

std::optional<Book::Level> Book::convert(const WireLevel& wire) const {
    const auto price = steps_of(wire.price, tick_size_);
    const auto qty = steps_of(wire.qty, lot_size_);
    if (!price.has_value() || !qty.has_value()) {
        return std::nullopt;
    }
    return Level{.price = PriceTicks::from_raw(*price), .qty = QtyLots::from_raw(*qty)};
}

void Book::upsert(book::Ladder& side, const Level& level) {
    if (level.qty.raw == 0) {
        // Size zero is Coinbase's delete: 1924 of 4092 live changes carry one.
        side.remove(level.price);
        return;
    }
    side.upsert(level);
}

BookResult Book::reset(const WireL2Snapshot& wire) {
    ladders_.clear();
    const auto take = [this](std::span<const WireLevel> src, book::Ladder& into) {
        into.reserve(src.size());
        for (const auto& entry : src) {
            const auto level = convert(entry);
            if (!level.has_value()) {
                return false;
            }
            into.upsert(*level);
        }
        return true;
    };
    // A snapshot with one level we cannot read is not one to chain onto: the
    // stream only sends changes, so the hole would never heal.
    if (!take(wire.bids, ladders_.bids()) || !take(wire.asks, ladders_.asks()) ||
        ladders_.bids().empty() || ladders_.asks().empty()) {
        invalidate();
        return BookResult::BadLevel;
    }
    band_low_ = ladders_.bids().levels().back().price;
    band_high_ = ladders_.asks().levels().back().price;
    synced_ = true;
    return BookResult::Resynced;
}

BookResult Book::apply(const WireL2Update& update) {
    if (!synced_) {
        // Changes before the snapshot have no book to change.
        return BookResult::Ignored;
    }
    for (const auto& change : update.changes) {
        const auto level = convert({.price = change.price, .qty = change.qty});
        if (!level.has_value()) {
            // A level we cannot convert is not one we may skip: from here the
            // book would disagree with Coinbase's permanently, and invisibly.
            invalidate();
            return BookResult::BadLevel;
        }
        if (change.bid ? level->price.raw < band_low_.raw : level->price.raw > band_high_.raw) {
            ++outside_band_;
            continue;
        }
        upsert(change.bid ? ladders_.bids() : ladders_.asks(), *level);
    }
    if (ladders_.bids().empty() || ladders_.asks().empty()) {
        // The price walked clean out of the band, so what is left is no longer
        // the top of the venue's book.
        invalidate();
        return BookResult::BadLevel;
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    // The sequence is left at zero on purpose: level2_batch publishes no
    // ordering token, and a number we invented would read as one Coinbase
    // stood behind.
    return ladders_.snapshot(instrument_, 0, ts_event, ts_init);
}

} // namespace raikiri::venues::coinbase
