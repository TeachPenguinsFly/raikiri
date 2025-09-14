#include <raikiri/venues/bitfinex/book.hpp>
#include <raikiri/venues/bitfinex/map.hpp>

namespace raikiri::venues::bitfinex {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

using JsonType = detail::ondemand::json_type;

WireLevel level_from(const std::array<std::string_view, k_level_arity>& fields) noexcept {
    WireLevel out{};
    out.price = fields[k_level_price];
    out.amount = fields[k_level_amount];
    (void)token_to_i64(fields[k_level_count], out.count);
    return out;
}

// The venue's own reader: parse_decimal_e9 refuses the exponents this venue
// writes. The exact-or-refuse rule itself is book::exact_steps.
std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_number_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

// The sign is the side here, never part of the quantity, so it comes off
// before anything is scaled. A negative reaching QtyLots would be a size no
// consumer could interpret.
struct SignedToken {
    std::string_view magnitude;
    bool negative{false};
};

SignedToken split_sign(std::string_view text) noexcept {
    SignedToken out{.magnitude = text, .negative = false};
    if (!out.magnitude.empty() && (out.magnitude.front() == '-' || out.magnitude.front() == '+')) {
        out.negative = out.magnitude.front() == '-';
        out.magnitude.remove_prefix(1);
    }
    return out;
}

} // namespace

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        return with_payload(
            frame,
            [&storage](detail::ondemand::value payload,
                       std::string_view tag) -> std::expected<WireBook, WireError> {
                if (!tag.empty()) {
                    return std::unexpected(WireError::WrongType);
                }
                detail::ondemand::array rows;
                if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                bool snapshot = false;
                bool decided = false;
                std::size_t count = 0;
                std::array<std::string_view, k_level_arity> flat{};
                std::size_t flat_len = 0;
                for (auto entry : rows) {
                    detail::ondemand::value value;
                    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    JsonType type{};
                    if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    if (!decided) {
                        snapshot = type == JsonType::array;
                        decided = true;
                    }
                    if (snapshot) {
                        if (count == k_max_levels) {
                            return std::unexpected(WireError::TooManyEntries);
                        }
                        const auto row = read_row<k_level_arity>(value);
                        if (!row.has_value()) {
                            return std::unexpected(row.error());
                        }
                        storage.levels[count] = level_from(row->fields);
                        ++count;
                        continue;
                    }
                    if (flat_len < k_level_arity) {
                        const auto token = token_of(value);
                        if (!token.has_value()) {
                            return std::unexpected(token.error());
                        }
                        flat[flat_len] = *token;
                        ++flat_len;
                    }
                }
                if (!snapshot) {
                    if (flat_len < k_level_arity) {
                        return std::unexpected(WireError::ShortArray);
                    }
                    storage.levels[0] = level_from(flat);
                    count = 1;
                }
                return WireBook{.snapshot = snapshot, .levels = {storage.levels.data(), count}};
            });
    });
}

Book::Book(InstrumentId instrument, FixedE9 tick_size, FixedE9 lot_size)
    : instrument_{instrument}, tick_size_{tick_size}, lot_size_{lot_size} {}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
}

bool Book::apply_level(const WireLevel& wire) {
    const auto price = steps_of(wire.price, tick_size_);
    if (!price.has_value()) {
        return false;
    }
    const auto amount = split_sign(wire.amount);
    auto& side = amount.negative ? ladders_.asks() : ladders_.bids();
    const auto price_ticks = PriceTicks::from_raw(*price);

    if (wire.count == 0) {
        // COUNT zero is the delete, and the amount beside it is a bare +1/-1
        // that says only which side to look on — never a size to rest here.
        side.remove(price_ticks);
        return true;
    }

    const auto qty = steps_of(amount.magnitude, lot_size_);
    if (!qty.has_value() || *qty == 0) {
        return false;
    }
    side.upsert(book::Level{price_ticks, QtyLots::from_raw(*qty)});
    return true;
}

BookResult Book::apply(const WireBook& update) {
    if (update.snapshot) {
        ladders_.clear();
        for (const auto& wire : update.levels) {
            if (!apply_level(wire)) {
                invalidate();
                return BookResult::BadLevel;
            }
        }
        synced_ = true;
        return BookResult::Resynced;
    }

    if (!synced_) {
        // A delta before the opening snapshot has no book to change.
        return BookResult::Ignored;
    }
    for (const auto& wire : update.levels) {
        if (!apply_level(wire)) {
            invalidate();
            return BookResult::BadLevel;
        }
    }
    return BookResult::Applied;
}

std::optional<BookSnapshot> Book::snapshot(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_) {
        return std::nullopt;
    }
    // venue_seq left at zero on purpose: this channel publishes no ordering
    // token at all, and a number we invented would read as one Bitfinex stood
    // behind.
    return ladders_.snapshot(instrument_, /*venue_seq=*/0, ts_event, ts_init);
}

std::optional<Bbo> Book::touch(UnixNanos ts_event, UnixNanos ts_init) const {
    if (!synced_ || ladders_.bids().empty() || ladders_.asks().empty()) {
        return std::nullopt;
    }
    const auto& bid = ladders_.bids().levels().front();
    const auto& ask = ladders_.asks().levels().front();
    Bbo out{};
    out.instrument = instrument_;
    out.bid = bid.price;
    out.ask = ask.price;
    out.bid_qty = bid.qty;
    out.ask_qty = ask.qty;
    out.ts_event = ts_event;
    out.ts_init = ts_init;
    return out;
}

} // namespace raikiri::venues::bitfinex
