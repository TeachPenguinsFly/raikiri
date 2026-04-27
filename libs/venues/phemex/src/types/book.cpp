#include <raikiri/venues/phemex/book.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::phemex {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<WireRealLevel, WireError> read_level(detail::ondemand::array pair, WireRealLevel) {
    WireRealLevel out{};
    std::size_t field = 0;
    for (auto item : pair) {
        detail::ondemand::value value;
        if (const auto ec = item.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto text = detail::str_of(value);
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        if (field == 0) {
            out.price = *text;
        } else if (field == 1) {
            out.qty = *text;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

std::expected<WireScaledLevel, WireError> read_level(detail::ondemand::array pair,
                                                     WireScaledLevel) {
    WireScaledLevel out{};
    std::size_t field = 0;
    for (auto item : pair) {
        std::int64_t value = 0;
        if (const auto ec = item.get_int64().get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field == 0) {
            out.price_ep = value;
        } else if (field == 1) {
            out.qty_ev = value;
        }
        ++field;
    }
    if (field < 2) {
        return std::unexpected(WireError::ShortArray);
    }
    return out;
}

template <typename Level>
std::expected<std::size_t, WireError>
read_side(detail::ondemand::object& book, std::string_view key, std::span<Level> into) {
    auto array = detail::req_array(book, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        const auto level = read_level(pair, Level{});
        if (!level.has_value()) {
            return std::unexpected(level.error());
        }
        into[count] = *level;
        ++count;
    }
    return count;
}

template <typename Level>
std::expected<WireBook<Level>, WireError>
parse_book(std::string_view frame, std::string_view key, LevelBuffer<Level>& storage) {
    return guarded([&]() -> std::expected<WireBook<Level>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireBook<Level> out{};
        const auto symbol = detail::req_str(*root, "symbol");
        const auto type = detail::req_str(*root, "type");
        if (!symbol.has_value() || !type.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.head.symbol = *symbol;
        out.head.snapshot = *type == "snapshot";
        const auto depth = detail::opt_u64(*root, "depth");
        const auto sequence = detail::opt_u64(*root, "sequence");
        const auto ts = detail::opt_u64(*root, "timestamp");
        if (!depth.has_value() || !sequence.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.head.depth = static_cast<std::uint32_t>(*depth);
        out.head.sequence = *sequence;
        out.head.ts_ns = *ts;
        auto book = detail::req_object(*root, key);
        if (!book.has_value()) {
            return std::unexpected(book.error());
        }
        const auto bids = read_side(*book, "bids", std::span<Level>{storage.bids});
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_side(*book, "asks", std::span<Level>{storage.asks});
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

void apply_side(std::span<const book::Level> levels, book::Ladder& side) {
    for (const auto& level : levels) {
        if (level.qty.raw == 0) {
            side.remove(level.price);
            continue;
        }
        side.upsert(level);
    }
}

} // namespace

std::expected<WireBook<WireRealLevel>, WireError>
parse_real_book(std::string_view frame, LevelBuffer<WireRealLevel>& storage) {
    return parse_book(frame, "orderbook_p", storage);
}

std::expected<WireBook<WireScaledLevel>, WireError>
parse_scaled_book(std::string_view frame, LevelBuffer<WireScaledLevel>& storage) {
    return parse_book(frame, "book", storage);
}

void Book::invalidate() noexcept {
    ladders_.clear();
    synced_ = false;
}

BookResult Book::apply(bool snapshot,
                       std::span<const Level> bids,
                       std::span<const Level> asks,
                       std::uint32_t depth,
                       std::uint64_t sequence) {
    if (snapshot) {
        ladders_.clear();
        ++replacements_;
    } else if (!synced_) {
        return BookResult::Ignored;
    }
    apply_side(bids, ladders_.bids());
    apply_side(asks, ladders_.asks());
    if (depth != 0) {
        ladders_.bids().truncate(depth);
        ladders_.asks().truncate(depth);
    }
    const auto touch = ladders_.touch();
    if (touch.has_value() && touch->first.price.raw >= touch->second.price.raw) {
        ++crossings_;
        invalidate();
        return BookResult::Crossed;
    }
    sequence_ = sequence;
    synced_ = true;
    return snapshot ? BookResult::Replaced : BookResult::Applied;
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
    return ladders_.snapshot(instrument_, sequence_, ts_event, ts_init);
}

} // namespace raikiri::venues::phemex
