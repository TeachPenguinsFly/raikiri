#include <raikiri/venues/bingx/book.hpp>
#include <raikiri/venues/bingx/envelope.hpp>

#include <algorithm>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

// A level is ["64148.0","7.4404"]: price then size, both strings.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::array pair;
    if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    std::size_t field = 0;
    for (auto element : pair) {
        detail::ondemand::value value;
        if (const auto ec = element.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (field < 2) {
            const auto text = detail::str_of(value);
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
read_levels(detail::ondemand::object& obj, std::string_view key, std::span<WireLevel> into) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
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

} // namespace

std::expected<WireBookTicker, WireError> parse_book_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBookTicker out{};
        const auto symbol = detail::req_str(*data, "s");
        const auto bid = detail::req_str(*data, "b");
        const auto bid_qty = detail::req_str(*data, "B");
        const auto ask = detail::req_str(*data, "a");
        const auto ask_qty = detail::req_str(*data, "A");
        if (!symbol.has_value() || !bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
            !ask_qty.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.bid_price = *bid;
        out.bid_qty = *bid_qty;
        out.ask_price = *ask;
        out.ask_qty = *ask_qty;
        // opt_* answers 0 for a field that is not there, which is what makes
        // this frame readable on both trees: the swap carries u and T, spot
        // carries neither, and only E is common. A zero here means ABSENT and
        // must not be read as a sequence number or a timestamp.
        const auto update_id = detail::opt_u64(*data, "u");
        const auto transaction = detail::opt_u64(*data, "T");
        const auto event = detail::opt_u64(*data, "E");
        if (!update_id.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.transaction_ms = *transaction;
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireDepth, WireError>
parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto data_type = detail::req_str(*root, "dataType");
        // The swap envelope stamps "ts"; the spot one stamps "timestamp".
        const auto ts = detail::opt_u64(*root, "ts");
        const auto stamp = detail::opt_u64(*root, "timestamp");
        if (!data_type.has_value() || !ts.has_value() || !stamp.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        WireDepth out{};
        out.symbol = symbol_of(*data_type);
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField); // nothing else names it
        }
        out.ts_ms = *ts != 0 ? *ts : *stamp;
        // SPOT SENDS ASKS WORST FIRST. Both trees send bids best-first, so the
        // asymmetry reads as a normal book right up to the point where the top
        // of it is a hundred dollars away from the bid — and crossed against
        // it, which is a signal every strategy acts on.
        if (market == MarketType::Spot) {
            std::reverse(storage.asks.begin(), storage.asks.begin() + static_cast<long>(*asks));
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::bingx
