#include "wire_detail.hpp"

namespace raikiri::venues::kucoin {

namespace {

using detail::guarded;
using detail::session;

// `subject` separates all five shapes, including the two sharing the
// /contract/instrument topic, so the topic is read only for the symbol.
EventKind kind_from_subject(std::string_view subject) noexcept {
    if (subject == "tickerV2") {
        return EventKind::Bbo;
    }
    if (subject == "match") {
        return EventKind::Trade;
    }
    if (subject == "level2") {
        return EventKind::Depth;
    }
    if (subject == "mark.index.price") {
        return EventKind::MarkPrice;
    }
    if (subject == "funding.rate") {
        return EventKind::FundingRate;
    }
    return EventKind::Unknown;
}

// "/contractMarket/tickerV2:XBTUSDTM" -> "XBTUSDTM". A batched subscription is
// echoed one symbol per frame (verified live), so this is never a list.
std::string_view symbol_of_topic(std::string_view topic) noexcept {
    const auto colon = topic.rfind(':');
    if (colon == std::string_view::npos) {
        return {};
    }
    topic.remove_prefix(colon + 1); // not substr: it throws, and this is noexcept
    return topic;
}

std::expected<std::string_view, WireError> topic_symbol(detail::ondemand::object& root) {
    const auto topic = detail::req_str(root, "topic");
    if (!topic.has_value()) {
        return std::unexpected(topic.error());
    }
    const auto symbol = symbol_of_topic(*topic);
    if (symbol.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    return symbol;
}

// "62234.6,buy,1" -> price, side, size. A partly-formed change would apply a
// real price at an invented size, so anything short of three fields fails.
std::expected<void, WireError> split_change(std::string_view change, WireBookChange& out) {
    const auto first = change.find(',');
    if (first == std::string_view::npos) {
        return std::unexpected(WireError::WrongType);
    }
    const auto second = change.find(',', first + 1);
    if (second == std::string_view::npos) {
        return std::unexpected(WireError::WrongType);
    }
    out.price = change.substr(0, first);
    const auto side = change.substr(first + 1, second - first - 1);
    if (side != "buy" && side != "sell") {
        return std::unexpected(WireError::WrongType);
    }
    out.bid = side == "buy";
    out.qty = change.substr(second + 1);
    if (out.price.empty() || out.qty.empty()) {
        return std::unexpected(WireError::MissingField);
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
        const auto type = detail::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        EventInfo info{};
        if (*type != "message") {
            if (*type == "welcome") {
                info.kind = EventKind::Welcome;
            } else if (*type == "ack") {
                info.kind = EventKind::Ack;
            } else if (*type == "pong") {
                info.kind = EventKind::Pong;
            } else if (*type == "error") {
                info.kind = EventKind::Error;
            }
            return info;
        }
        const auto subject = detail::req_str(*root, "subject");
        if (!subject.has_value()) {
            return std::unexpected(subject.error());
        }
        info.kind = kind_from_subject(*subject);
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        info.symbol = *symbol;
        return info;
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // `code` is a bare number and the text sits in `data`; the REST
        // envelope spells both the other way round.
        const auto code = detail::opt_i64(*root, "code");
        const auto msg = detail::opt_str(*root, "data");
        if (!code.has_value() || !msg.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return StreamError{.code = *code, .msg = *msg};
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
                WireBbo out{};
                out.symbol = *symbol;
                const auto bid = detail::req_decimal(obj, "bestBidPrice");
                const auto bid_qty = detail::req_decimal(obj, "bestBidSize");
                const auto ask = detail::req_decimal(obj, "bestAskPrice");
                const auto ask_qty = detail::req_decimal(obj, "bestAskSize");
                if (!bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
                    !ask_qty.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.bid_price = *bid;
                out.bid_qty = *bid_qty;
                out.ask_price = *ask;
                out.ask_qty = *ask_qty;
                const auto ts = detail::opt_u64(obj, "ts");
                const auto sequence = detail::opt_i64(obj, "sequence");
                if (!ts.has_value() || !sequence.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ns = *ts;
                out.sequence = *sequence;
                return out;
            });
    });
}

std::expected<WireTrade, WireError> parse_trade(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTrade, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireTrade, WireError> {
                WireTrade out{};
                out.symbol = *symbol;
                const auto trade_id = detail::req_str(obj, "tradeId");
                const auto price = detail::req_decimal(obj, "price");
                const auto qty = detail::req_decimal(obj, "size");
                const auto side = detail::req_str(obj, "side");
                if (!trade_id.has_value() || !price.has_value() || !qty.has_value() ||
                    !side.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.trade_id = *trade_id;
                out.price = *price;
                out.qty = *qty;
                out.aggressor_bought = *side == "buy";
                const auto ts = detail::opt_u64(obj, "ts");
                const auto sequence = detail::opt_i64(obj, "sequence");
                if (!ts.has_value() || !sequence.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ns = *ts;
                out.sequence = *sequence;
                return out;
            });
    });
}

std::expected<WireBookChange, WireError> parse_book_change(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBookChange, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBookChange, WireError> {
                WireBookChange out{};
                out.symbol = *symbol;
                const auto change = detail::req_str(obj, "change");
                if (!change.has_value()) {
                    return std::unexpected(change.error());
                }
                if (const auto ok = split_change(*change, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                const auto ts = detail::opt_u64(obj, "timestamp");
                const auto sequence = detail::opt_i64(obj, "sequence");
                if (!ts.has_value() || !sequence.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.ts_ms = *ts;
                out.sequence = *sequence;
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
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireMarkPrice, WireError> {
                WireMarkPrice out{};
                out.symbol = *symbol;
                const auto mark = detail::req_decimal(obj, "markPrice");
                const auto index = detail::opt_decimal(obj, "indexPrice");
                if (!mark.has_value() || !index.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.mark_price = *mark;
                out.index_price = *index;
                const auto ts = detail::opt_u64(obj, "timestamp");
                if (!ts.has_value()) {
                    return std::unexpected(ts.error());
                }
                out.ts_ms = *ts;
                return out;
            });
    });
}

std::expected<WireFundingRate, WireError> parse_funding_rate(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFundingRate, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto symbol = topic_symbol(*root);
        if (!symbol.has_value()) {
            return std::unexpected(symbol.error());
        }
        return detail::with_data(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireFundingRate, WireError> {
                WireFundingRate out{};
                out.symbol = *symbol;
                const auto rate = detail::req_decimal(obj, "fundingRate");
                if (!rate.has_value()) {
                    return std::unexpected(rate.error());
                }
                out.rate = *rate;
                const auto granularity = detail::opt_u64(obj, "granularity");
                const auto ts = detail::opt_u64(obj, "timestamp");
                if (!granularity.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::WrongType);
                }
                out.granularity_ms = *granularity;
                out.ts_ms = *ts;
                return out;
            });
    });
}

} // namespace raikiri::venues::kucoin
