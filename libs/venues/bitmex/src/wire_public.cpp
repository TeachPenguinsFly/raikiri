#include "wire_detail.hpp"

namespace raikiri::venues::bitmex {

namespace {

using detail::guarded;
using detail::session;

TableKind table_from_name(std::string_view table) noexcept {
    if (table == "quote") {
        return TableKind::Quote;
    }
    if (table == "trade") {
        return TableKind::Trade;
    }
    if (table == "instrument") {
        return TableKind::Instrument;
    }
    if (table == "orderBookL2_25") {
        return TableKind::OrderBook;
    }
    return TableKind::Unknown;
}

FrameAction action_from_name(std::string_view action) noexcept {
    if (action == "partial") {
        return FrameAction::Partial;
    }
    if (action == "insert") {
        return FrameAction::Insert;
    }
    if (action == "update") {
        return FrameAction::Update;
    }
    if (action == "delete") {
        return FrameAction::Delete;
    }
    return FrameAction::None;
}

// Both sides of a quote are optional, but a price without its size is not: the
// size that stayed behind belongs to the old price and pairing them would put a
// stale quantity on a fresh level.
[[nodiscard]] bool side_is_paired(std::string_view price, std::string_view qty) noexcept {
    return price.empty() == qty.empty();
}

} // namespace

std::expected<FrameInfo, WireError> parse_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<FrameInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto table = detail::opt_str(*root, "table");
        if (!table.has_value()) {
            return std::unexpected(table.error());
        }
        FrameInfo info{};
        if (!table->empty()) {
            const auto action = detail::opt_str(*root, "action");
            if (!action.has_value()) {
                return std::unexpected(action.error());
            }
            info.table = table_from_name(*table);
            info.action = action_from_name(*action);
            return info;
        }
        const auto error = detail::opt_raw(*root, "error");
        const auto success = detail::opt_raw(*root, "success");
        const auto banner = detail::opt_raw(*root, "info");
        if (!error.has_value() || !success.has_value() || !banner.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        if (!error->empty()) {
            info.table = TableKind::Error;
        } else if (!success->empty()) {
            info.table = TableKind::SubscribeAck;
        } else if (!banner->empty()) {
            info.table = TableKind::Info;
        }
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto message = detail::req_str(*root, "error");
        const auto status = detail::opt_i64(*root, "status");
        if (!message.has_value() || !status.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.status = *status, .message = *message};
    });
}

std::expected<SubscribeAck, WireError> parse_subscribe_ack(std::string_view frame) {
    return guarded([&]() -> std::expected<SubscribeAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto topic = detail::req_str(*root, "subscribe");
        const auto success = detail::opt_bool(*root, "success");
        if (!topic.has_value() || !success.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return SubscribeAck{.topic = *topic, .success = *success};
    });
}

std::expected<std::span<const WireQuote>, WireError> parse_quotes(std::string_view frame,
                                                                  QuoteBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireQuote>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_quotes) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireQuote out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto pool = detail::opt_str(obj, "pool");
            const auto ts = detail::opt_str(obj, "timestamp");
            const auto bid_price = detail::opt_num(obj, "bidPrice");
            const auto bid_qty = detail::opt_num(obj, "bidSize");
            const auto ask_price = detail::opt_num(obj, "askPrice");
            const auto ask_qty = detail::opt_num(obj, "askSize");
            if (!symbol.has_value() || !pool.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (!bid_price.has_value() || !bid_qty.has_value() || !ask_price.has_value() ||
                !ask_qty.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            if (!side_is_paired(*bid_price, *bid_qty) || !side_is_paired(*ask_price, *ask_qty)) {
                return std::unexpected(WireError::MissingField);
            }
            if (bid_price->empty() && ask_price->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.pool = *pool;
            out.ts = *ts;
            out.bid_price = *bid_price;
            out.bid_qty = *bid_qty;
            out.ask_price = *ask_price;
            out.ask_qty = *ask_qty;
            storage.quotes[count] = out;
            ++count;
        }
        return std::span<const WireQuote>{storage.quotes.data(), count};
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireTrade out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto side = detail::req_str(obj, "side");
            const auto ts = detail::req_str(obj, "timestamp");
            const auto trade_id = detail::opt_str(obj, "trdMatchID");
            const auto price = detail::opt_num(obj, "price");
            const auto qty = detail::opt_num(obj, "size");
            if (!symbol.has_value() || !side.has_value() || !ts.has_value() ||
                !trade_id.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (!price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            if (price->empty() || qty->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.symbol = *symbol;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "Buy";
            out.ts = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireInstrumentTick, WireError> parse_instrument_tick(std::string_view frame) {
    return guarded([&]() -> std::expected<WireInstrumentTick, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_first_data(
            *root,
            [](detail::ondemand::object& obj) -> std::expected<WireInstrumentTick, WireError> {
                WireInstrumentTick out{};
                const auto symbol = detail::req_str(obj, "symbol");
                if (!symbol.has_value()) {
                    return std::unexpected(symbol.error());
                }
                out.symbol = *symbol;
                const auto ts = detail::opt_str(obj, "timestamp");
                const auto funding_ts = detail::opt_str(obj, "fundingTimestamp");
                const auto interval = detail::opt_str(obj, "fundingInterval");
                const auto mark = detail::opt_num(obj, "markPrice");
                const auto rate = detail::opt_num(obj, "fundingRate");
                const auto indicative = detail::opt_num(obj, "indicativeFundingRate");
                if (!ts.has_value() || !funding_ts.has_value() || !interval.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                if (!mark.has_value() || !rate.has_value() || !indicative.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts = *ts;
                out.funding_ts = *funding_ts;
                out.funding_interval = *interval;
                out.mark_price = *mark;
                out.funding_rate = *rate;
                out.indicative_funding_rate = *indicative;
                return out;
            });
    });
}

std::expected<std::span<const WireLevel>, WireError> parse_book(std::string_view frame,
                                                                LevelBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireLevel>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto array = detail::req_array(*root, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_levels) {
                return std::unexpected(WireError::TooManyEntries);
            }
            detail::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            WireLevel out{};
            const auto symbol = detail::req_str(obj, "symbol");
            const auto side = detail::req_str(obj, "side");
            const auto id = detail::opt_i64(obj, "id");
            const auto price = detail::opt_num(obj, "price");
            const auto qty = detail::opt_num(obj, "size");
            if (!symbol.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            if (!id.has_value() || !price.has_value() || !qty.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.symbol = *symbol;
            out.id = *id;
            out.bid = *side == "Buy";
            out.price = *price;
            out.qty = *qty;
            storage.levels[count] = out;
            ++count;
        }
        return std::span<const WireLevel>{storage.levels.data(), count};
    });
}

} // namespace raikiri::venues::bitmex
