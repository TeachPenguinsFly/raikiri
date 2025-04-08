#include "wire_detail.hpp"

namespace raikiri::venues::okx {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bbo-tbt") {
        return EventKind::Bbo;
    }
    if (channel == "trades") {
        return EventKind::Trade;
    }
    if (channel == "funding-rate") {
        return EventKind::FundingRate;
    }
    if (channel == "mark-price") {
        return EventKind::MarkPrice;
    }
    // books5 and books-l2-tbt exist and carry a different shape; only the one
    // ws.cpp subscribes to is claimed here.
    if (channel == "books") {
        return EventKind::Books;
    }
    return EventKind::Unknown;
}

// bbo-tbt and books identify the instrument only in the frame's `arg` — the
// data elements carry no instId at all, unlike trades and funding-rate.
std::expected<std::string_view, WireError> arg_inst_id(detail::ondemand::object& root) {
    auto arg = detail::req_object(root, "arg");
    if (!arg.has_value()) {
        return std::unexpected(arg.error());
    }
    return detail::req_str(*arg, "instId");
}

// A level is ["64043.6","1282.19","0","57"]: price, size, a deprecated field
// pinned to "0", and the order count. Only the first two are read.
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

// Top of book only. An empty side is a real state on an illiquid contract, so
// it yields empty views rather than an error; the mapper drops those.
std::expected<void, WireError> read_top(detail::ondemand::object& obj,
                                        std::string_view key,
                                        std::string_view& price,
                                        std::string_view& qty) {
    auto array = detail::req_array(obj, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
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
            // Control frame. "error" is a refused subscription, everything
            // else (subscribe, unsubscribe, channel-conn-count) is an
            // acknowledgement we count but do not act on.
            info.kind = *event == "error" ? EventKind::Error : EventKind::SubscribeAck;
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
        const auto msg = detail::opt_str(*root, "msg");
        const auto code = detail::opt_str(*root, "code");
        if (!msg.has_value() || !code.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = static_cast<std::int64_t>(detail::digits_to_u64(*code)),
                           .msg = *msg};
    });
}

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
        return detail::with_first_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
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
                const auto ts = detail::opt_ms(obj, "ts");
                const auto seq = detail::opt_i64(obj, "seqId");
                if (!ts.has_value() || !seq.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ms = *ts;
                out.seq_id = *seq;
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
            const auto inst_id = detail::req_str(obj, "instId");
            const auto trade_id = detail::opt_str(obj, "tradeId");
            const auto price = detail::req_str(obj, "px");
            const auto qty = detail::req_str(obj, "sz");
            const auto side = detail::req_str(obj, "side");
            if (!inst_id.has_value() || !trade_id.has_value() || !price.has_value() ||
                !qty.has_value() || !side.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.inst_id = *inst_id;
            out.trade_id = *trade_id;
            out.price = *price;
            out.qty = *qty;
            out.aggressor_bought = *side == "buy";
            const auto ts = detail::opt_ms(obj, "ts");
            const auto seq = detail::opt_i64(obj, "seqId");
            if (!ts.has_value() || !seq.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            out.ts_ms = *ts;
            out.seq_id = *seq;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                const auto inst_id = detail::req_str(obj, "instId");
                if (!inst_id.has_value()) {
                    return std::unexpected(inst_id.error());
                }
                out.inst_id = *inst_id;
                const auto rate = detail::opt_str(obj, "fundingRate");
                const auto next_rate = detail::opt_str(obj, "nextFundingRate");
                if (!rate.has_value() || !next_rate.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.funding_rate = *rate;
                out.next_funding_rate = *next_rate;
                const auto funding = detail::opt_ms(obj, "fundingTime");
                const auto next = detail::opt_ms(obj, "nextFundingTime");
                const auto ts = detail::opt_ms(obj, "ts");
                if (!funding.has_value() || !next.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.funding_ms = *funding;
                out.next_funding_ms = *next;
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireMarkPrice, WireError> parse_mark_price(std::string_view frame) {
    return guarded([&]() -> std::expected<WireMarkPrice, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_first_data(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                const auto inst_id = detail::req_str(obj, "instId");
                const auto mark = detail::req_str(obj, "markPx");
                if (!inst_id.has_value() || !mark.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.inst_id = *inst_id;
                out.mark_price = *mark;
                const auto ts = detail::opt_ms(obj, "ts");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
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
                const auto bids = read_levels(obj, "bids", storage.bids);
                if (!bids.has_value()) {
                    return std::unexpected(bids.error());
                }
                const auto asks = read_levels(obj, "asks", storage.asks);
                if (!asks.has_value()) {
                    return std::unexpected(asks.error());
                }
                const auto ts = detail::opt_ms(obj, "ts");
                const auto seq = detail::opt_i64(obj, "seqId");
                const auto prev = detail::opt_i64(obj, "prevSeqId");
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

} // namespace raikiri::venues::okx
