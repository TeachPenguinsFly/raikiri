#include <raikiri/venues/lighter/book.hpp>
#include <raikiri/venues/lighter/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::lighter {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<std::size_t, WireError>
read_levels(detail::ondemand::object& book, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(book, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto price = detail::req_str(obj, "price");
        const auto size = detail::req_str(obj, "size");
        if (!price.has_value() || !size.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        into[count] = WireLevel{.price = *price, .size = *size};
        ++count;
    }
    return count;
}

std::optional<std::int64_t> steps_of(std::string_view text, FixedE9 step) {
    const auto value = parse_decimal_e9(text);
    return value.has_value() ? book::exact_steps(*value, step) : std::nullopt;
}

} // namespace

std::expected<WireOrderBook, WireError> parse_order_book(std::string_view frame,
                                                         LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireOrderBook out{};
        const auto type = detail::req_str(*root, "type");
        const auto channel = detail::req_str(*root, "channel");
        if (!type.has_value() || !channel.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.snapshot = type->starts_with("subscribed/");
        const auto market = channel_market(*channel);
        if (!market.has_value()) {
            return std::unexpected(market.error());
        }
        out.market_id = *market;
        auto book = detail::req_object(*root, "order_book");
        if (!book.has_value()) {
            return std::unexpected(book.error());
        }
        const auto bids = read_levels(*book, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*book, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        // A missing nonce fails the parse rather than reading as zero.
        if (!detail::has_field(*book, "nonce")) {
            return std::unexpected(WireError::MissingField);
        }
        const auto nonce = detail::opt_u64(*book, "nonce");
        const auto begin = detail::opt_u64(*book, "begin_nonce");
        const auto updated = detail::opt_u64(*book, "last_updated_at");
        if (!nonce.has_value() || !begin.has_value() || !updated.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.nonce = *nonce;
        out.begin_nonce = *begin;
        out.updated_us = *updated;
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
    nonce_ = 0;
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

BookResult Book::apply(const WireOrderBook& frame) {
    if (frame.snapshot) {
        ladders_.clear();
        if (!apply_side(frame.bids, ladders_.bids()) || !apply_side(frame.asks, ladders_.asks())) {
            invalidate();
            return BookResult::BadLevel;
        }
        nonce_ = frame.nonce;
        synced_ = true;
        return BookResult::Resynced;
    }
    if (!synced_) {
        return BookResult::Ignored;
    }
    if (frame.begin_nonce != nonce_) {
        if (frame.nonce <= nonce_) {
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
    nonce_ = frame.nonce;
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
    return ladders_.snapshot(instrument_, nonce_, ts_event, ts_init);
}

} // namespace raikiri::venues::lighter
