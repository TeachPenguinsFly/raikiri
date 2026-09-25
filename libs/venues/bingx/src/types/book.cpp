#include <raikiri/venues/bingx/book.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <algorithm>
#include <optional>

namespace raikiri::venues::bingx {

namespace {

using wire::guarded;
using wire::session;

// a level is ["64148.0","7.4404"]: price then size
std::expected<WireLevel, WireError> read_level(wire::ondemand::value entry, const wire::Session& s) {
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
            const auto text = wire::str_of(value, s);
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

std::expected<std::size_t, WireError> read_levels(wire::ondemand::value side, std::span<WireLevel> into) {
    wire::ondemand::array array;
    if (const auto ec = side.get_array().get(array); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    const auto& s = session();
    std::size_t count = 0;
    for (auto entry : array) {
        if (count == k_max_levels) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto level = read_level(value, s);
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
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBookTicker out{};
        using Text = std::expected<std::string_view, WireError>;
        using Number = std::expected<std::uint64_t, WireError>;
        Text symbol{std::unexpected(WireError::MissingField)};
        Text bid{std::unexpected(WireError::MissingField)};
        Text bid_qty{std::unexpected(WireError::MissingField)};
        Text ask{std::unexpected(WireError::MissingField)};
        Text ask_qty{std::unexpected(WireError::MissingField)};
        Number update_id{0};
        Number transaction{0};
        Number event{0};
        const auto walked = wire::for_each_field(*data, [&](std::string_view key, wire::ondemand::value& value) {
            if (key == "s") {
                symbol = wire::str_of(value);
            } else if (key == "b") {
                bid = wire::str_of(value);
            } else if (key == "B") {
                bid_qty = wire::str_of(value);
            } else if (key == "a") {
                ask = wire::str_of(value);
            } else if (key == "A") {
                ask_qty = wire::str_of(value);
            } else if (key == "u") {
                update_id = wire::u64_of(value);
            } else if (key == "T") {
                transaction = wire::u64_of(value);
            } else if (key == "E") {
                event = wire::u64_of(value);
            }
        });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        for (const auto* text : {&symbol, &bid, &bid_qty, &ask, &ask_qty}) {
            if (!text->has_value() || (*text)->empty()) {
                return std::unexpected(WireError::MissingField);
            }
        }
        out.symbol = *symbol;
        out.bid_price = *bid;
        out.bid_qty = *bid_qty;
        out.ask_price = *ask;
        out.ask_qty = *ask_qty;
        // zero means absent: swap sends u and T, spot neither
        if (!update_id.has_value() || !transaction.has_value() || !event.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.update_id = *update_id;
        out.transaction_ms = *transaction;
        out.event_ms = *event;
        return out;
    });
}

std::expected<WireDepth, WireError> parse_depth(std::string_view frame, LevelBuffer& storage, MarketType market) {
    return guarded([&]() -> std::expected<WireDepth, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        std::expected<std::string_view, WireError> data_type{std::unexpected(WireError::MissingField)};
        std::expected<std::uint64_t, WireError> ts{0};
        std::expected<std::uint64_t, WireError> stamp{0};
        std::optional<std::size_t> bids;
        std::optional<std::size_t> asks;
        std::expected<void, WireError> data{std::unexpected(WireError::MissingField)};
        const auto read_side = [&](std::string_view key,
                                   wire::ondemand::value& value) -> std::expected<void, WireError> {
            if (key == "bids" || key == "asks") {
                const bool bid = key == "bids";
                const auto count = read_levels(value, bid ? storage.bids : storage.asks);
                if (!count.has_value()) {
                    return std::unexpected(count.error());
                }
                (bid ? bids : asks) = *count;
            }
            return {};
        };
        const auto walked = wire::for_each_field(
            *root, [&](std::string_view key, wire::ondemand::value& value) -> std::expected<void, WireError> {
                if (key == "data") {
                    wire::ondemand::object obj;
                    if (const auto ec = value.get_object().get(obj); ec != simdjson::SUCCESS) {
                        data = std::unexpected(wire::map_error(ec));
                        return {};
                    }
                    data = wire::for_each_field(obj, read_side);
                    return data;
                }
                if (key == "dataType") {
                    data_type = wire::str_of(value);
                } else if (key == "ts") {
                    ts = wire::u64_of(value);
                } else if (key == "timestamp") {
                    stamp = wire::u64_of(value);
                }
                return {};
            });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        if (!data_type.has_value() || data_type->empty() || !ts.has_value() || !stamp.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        if (!bids.has_value() || !asks.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireDepth out{};
        out.symbol = symbol_of(*data_type);
        if (out.symbol.empty()) {
            return std::unexpected(WireError::MissingField); // nothing else names it
        }
        out.ts_ms = *ts != 0 ? *ts : *stamp;
        // spot sends asks worst first
        if (market == MarketType::Spot) {
            std::reverse(storage.asks.begin(), storage.asks.begin() + static_cast<long>(*asks));
        }
        out.bids = {storage.bids.data(), *bids};
        out.asks = {storage.asks.data(), *asks};
        return out;
    });
}

} // namespace raikiri::venues::bingx
