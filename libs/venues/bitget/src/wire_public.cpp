#include "wire_detail.hpp"

namespace raikiri::venues::bitget {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "ticker") {
        return EventKind::Ticker;
    }
    if (channel == "trade") {
        return EventKind::Trade;
    }
    // Capped snapshots and the incremental 500-level feed ship one element.
    if (channel == "books5" || channel == "books15" || channel == "books") {
        return EventKind::Books;
    }
    if (channel == "orders") {
        return EventKind::Orders;
    }
    return EventKind::Unknown;
}

// trade and books elements carry no instrument id at all, so `arg` is the
// only place every channel names it.
std::expected<std::string_view, WireError> arg_inst_id(detail::ondemand::object& root) {
    auto arg = detail::req_object(root, "arg");
    if (!arg.has_value()) {
        return std::unexpected(arg.error());
    }
    return detail::req_str(*arg, "instId");
}

} // namespace

std::expected<EventInfo, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto event = detail::opt_str(*root, "event");
        if (!event.has_value()) {
            return std::unexpected(event.error());
        }
        EventInfo info{};
        if (!event->empty()) {
            // The login answer must not be read as a subscribe ack: it is the
            // one control frame whose absence means the private socket is
            // connected and deaf.
            info.kind = *event == "error"     ? EventKind::Error
                        : *event == "login"   ? EventKind::LoginAck
                                              : EventKind::SubscribeAck;
            return info;
        }
        auto arg = detail::req_object(*root, "arg");
        if (!arg.has_value()) {
            return std::unexpected(arg.error());
        }
        const auto channel = detail::req_str(*arg, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        info.kind = kind_from_channel(*channel);
        const auto inst_id = detail::opt_str(*arg, "instId");
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        info.inst_id = *inst_id;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = detail::opt_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "msg");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg};
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
        }
        return detail::with_first_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireTicker, WireError> {
                WireTicker out{};
                out.inst_id = *inst_id;
                const auto bid = detail::req_str(obj, "bidPr");
                const auto bid_qty = detail::req_str(obj, "bidSz");
                const auto ask = detail::req_str(obj, "askPr");
                const auto ask_qty = detail::req_str(obj, "askSz");
                if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
                    !ask_qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.bid_price = *bid;
                out.bid_qty = *bid_qty;
                out.ask_price = *ask;
                out.ask_qty = *ask_qty;
                // Optional: no funding rate must not cost the BBO with it.
                const auto last = detail::opt_str(obj, "lastPr");
                const auto mark = detail::opt_str(obj, "markPrice");
                const auto index = detail::opt_str(obj, "indexPrice");
                const auto rate = detail::opt_str(obj, "fundingRate");
                if (!last.has_value() || !mark.has_value() || !index.has_value() ||
                    !rate.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.last_price = *last;
                out.mark_price = *mark;
                out.index_price = *index;
                out.funding_rate = *rate;
                const auto next = detail::opt_ms(obj, "nextFundingTime");
                const auto ts = detail::opt_ms(obj, "ts");
                if (!next.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.next_funding_ms = *next;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto inst_id = arg_inst_id(*root);
        if (!inst_id.has_value()) {
            return std::unexpected(inst_id.error());
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
            out.inst_id = *inst_id;
            const auto trade_id = detail::opt_str(obj, "tradeId");
            const auto price = detail::req_str(obj, "price");
            const auto qty = detail::req_str(obj, "size");
            const auto side = detail::req_str(obj, "side");
            if (!trade_id.has_value() || !price.has_value() || !qty.has_value() ||
                !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "buy";
            const auto ts = detail::opt_ms(obj, "ts");
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            out.ts_ms = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
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
        const auto action = detail::opt_str(*root, "action");
        if (!action.has_value()) {
            return std::unexpected(action.error());
        }
        const bool snapshot = *action == "snapshot";
        return detail::with_first_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBooks, WireError> {
                WireBooks out{};
                out.inst_id = *inst_id;
                out.snapshot = snapshot;
                if (const auto ok = detail::read_book_body(obj, storage, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                return out;
            });
    });
}

} // namespace raikiri::venues::bitget
