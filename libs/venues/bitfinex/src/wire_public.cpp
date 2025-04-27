#include "wire_detail.hpp"

namespace raikiri::venues::bitfinex {

namespace {

using detail::guarded;
using detail::session;

using JsonType = detail::ondemand::json_type;

FrameKind kind_from_event(std::string_view event) noexcept {
    if (event == "info") {
        return FrameKind::Info;
    }
    if (event == "subscribed") {
        return FrameKind::Subscribed;
    }
    if (event == "unsubscribed") {
        return FrameKind::Unsubscribed;
    }
    if (event == "error") {
        return FrameKind::Error;
    }
    if (event == "pong") {
        return FrameKind::Pong;
    }
    // The `conf` acknowledgement lands here: nothing acts on it beyond having
    // asked for it.
    return FrameKind::Unknown;
}

// Walks a data envelope to its payload and hands it to `fn` along with the
// string tag that preceded it, if any.
//
// The envelope is variable-length BY DESIGN and is the one array here that
// must not be read with a fixed arity: `[CHAN_ID,<payload>]`,
// `[CHAN_ID,"te",<payload>]`, and either of those with a trailing send
// timestamp once the `conf` TIMESTAMP flag is on. Position 1 is therefore
// typed, not counted — a string there is a tag and an array is the payload.
template <typename Fn>
[[nodiscard]] auto with_payload(std::string_view frame, Fn&& fn)
    -> decltype(fn(std::declval<detail::ondemand::value>(), std::string_view{})) {
    auto root = session().open_array(frame);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::size_t index = 0;
    std::string_view tag;
    for (auto entry : *root) {
        detail::ondemand::value value;
        if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (index == 0) {
            ++index; // channel id; the caller already dispatched on it
            continue;
        }
        JsonType type{};
        if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        if (index == 1 && type == JsonType::string) {
            const auto token = detail::token_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            tag = *token;
            ++index;
            continue;
        }
        if (type != JsonType::array) {
            return std::unexpected(WireError::WrongType);
        }
        return std::forward<Fn>(fn)(value, tag);
    }
    return std::unexpected(WireError::MissingField);
}

std::expected<WireTrade, WireError> read_trade(detail::ondemand::value value) {
    const auto row = detail::read_row<k_trade_arity>(value);
    if (!row.has_value()) {
        return std::unexpected(row.error());
    }
    WireTrade out{};
    out.id = row->template at<k_trade_id>();
    out.ts_ms = detail::digits_to_u64(row->template at<k_trade_mts>());
    out.amount = row->template at<k_trade_amount>();
    out.price = row->template at<k_trade_price>();
    return out;
}

WireLevel level_from(const std::array<std::string_view, k_level_arity>& fields) noexcept {
    WireLevel out{};
    out.price = fields[k_level_price];
    out.amount = fields[k_level_amount];
    (void)detail::token_to_i64(fields[k_level_count], out.count);
    return out;
}

} // namespace

std::expected<FrameInfo, WireError> parse_frame(std::string_view frame) {
    return guarded([&]() -> std::expected<FrameInfo, WireError> {
        FrameInfo info{};
        if (!detail::is_array_frame(frame)) {
            auto root = session().open_object(frame);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            const auto event = detail::opt_str(*root, "event");
            const auto code = detail::opt_i64(*root, "code");
            if (!event.has_value() || !code.has_value()) {
                return std::unexpected(WireError::WrongType);
            }
            info.kind = kind_from_event(*event);
            info.code = *code;
            return info;
        }

        auto root = session().open_array(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // The whole envelope is walked, but the payload is stepped OVER
        // rather than into: peeking an element's type and never asking for its
        // contents leaves simdjson to skip the subtree structurally. That is
        // what makes reading a trailing timestamp cost nothing.
        std::size_t index = 0;
        for (auto entry : *root) {
            detail::ondemand::value value;
            if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (index == 0) {
                const auto token = detail::token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                if (!detail::token_to_i64(*token, info.chan_id)) {
                    return std::unexpected(WireError::WrongType);
                }
                ++index;
                continue;
            }
            JsonType type{};
            if (const auto ec = value.type().get(type); ec != simdjson::SUCCESS) {
                return std::unexpected(detail::map_error(ec));
            }
            if (index == 1 && type == JsonType::string) {
                const auto token = detail::token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                info.tag = *token;
            } else if (type == JsonType::number) {
                const auto token = detail::token_of(value);
                if (!token.has_value()) {
                    return std::unexpected(token.error());
                }
                info.ts_ms = detail::digits_to_u64(*token);
            }
            ++index;
        }
        // A bare `[123]` names a channel and says nothing about it. On a
        // positional wire that is not an empty update, it is a truncated one.
        if (index < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        // "hb" occupies the same slot a trade tag does, so a dispatcher that
        // does not single it out feeds a heartbeat to a payload parser.
        info.kind = info.tag == "hb" ? FrameKind::Heartbeat : FrameKind::Data;
        return info;
    });
}

std::expected<WireSubscribed, WireError> parse_subscribed(std::string_view frame) {
    return guarded([&]() -> std::expected<WireSubscribed, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto chan_id = detail::opt_i64(*root, "chanId");
        const auto channel = detail::opt_str(*root, "channel");
        const auto symbol = detail::opt_str(*root, "symbol");
        const auto key = detail::opt_str(*root, "key");
        if (!chan_id.has_value() || !channel.has_value() || !symbol.has_value() ||
            !key.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        // Without a channel id the ack binds nothing, and an unbound channel's
        // data frames are unattributable for the life of the connection.
        if (*chan_id == 0 || channel->empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireSubscribed{
            .chan_id = *chan_id, .channel = *channel, .symbol = *symbol, .key = *key};
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
        return with_payload(frame,
                            [](detail::ondemand::value payload,
                               std::string_view tag) -> std::expected<WireTicker, WireError> {
                                if (!tag.empty()) {
                                    return std::unexpected(WireError::WrongType);
                                }
                                const auto row = detail::read_row<k_ticker_arity>(payload);
                                if (!row.has_value()) {
                                    return std::unexpected(row.error());
                                }
                                WireTicker out{};
                                out.bid = row->at<k_ticker_bid>();
                                out.bid_size = row->at<k_ticker_bid_size>();
                                out.ask = row->at<k_ticker_ask>();
                                out.ask_size = row->at<k_ticker_ask_size>();
                                out.last_price = row->at<k_ticker_last_price>();
                                return out;
                            });
    });
}

std::expected<WireTrades, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<WireTrades, WireError> {
        return with_payload(
            frame,
            [&storage](detail::ondemand::value payload,
                       std::string_view tag) -> std::expected<WireTrades, WireError> {
                if (tag == "te" || tag == "tu") {
                    const auto trade = read_trade(payload);
                    if (!trade.has_value()) {
                        return std::unexpected(trade.error());
                    }
                    storage.trades[0] = *trade;
                    return WireTrades{.kind = tag == "te" ? TradeFrameKind::Executed
                                                          : TradeFrameKind::Updated,
                                      .trades = {storage.trades.data(), 1}};
                }
                if (!tag.empty()) {
                    return std::unexpected(WireError::WrongType);
                }
                detail::ondemand::array rows;
                if (const auto ec = payload.get_array().get(rows); ec != simdjson::SUCCESS) {
                    return std::unexpected(detail::map_error(ec));
                }
                std::size_t count = 0;
                for (auto entry : rows) {
                    if (count == k_max_trades) {
                        return std::unexpected(WireError::TooManyEntries);
                    }
                    detail::ondemand::value value;
                    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    const auto trade = read_trade(value);
                    if (!trade.has_value()) {
                        return std::unexpected(trade.error());
                    }
                    storage.trades[count] = *trade;
                    ++count;
                }
                return WireTrades{.kind = TradeFrameKind::Snapshot,
                                  .trades = {storage.trades.data(), count}};
            });
    });
}

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
                // No tag distinguishes a book snapshot from a single-level
                // delta; only the TYPE of the payload's first element does.
                // On-Demand is forward-only, so the flat case is collected as
                // it is walked rather than re-read once the shape is known.
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
                        const auto row = detail::read_row<k_level_arity>(value);
                        if (!row.has_value()) {
                            return std::unexpected(row.error());
                        }
                        storage.levels[count] = level_from(row->fields);
                        ++count;
                        continue;
                    }
                    if (flat_len < k_level_arity) {
                        const auto token = detail::token_of(value);
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

std::expected<WireDerivStatus, WireError> parse_deriv_status(std::string_view frame) {
    return guarded([&]() -> std::expected<WireDerivStatus, WireError> {
        return with_payload(frame,
                            [](detail::ondemand::value payload,
                               std::string_view tag) -> std::expected<WireDerivStatus, WireError> {
                                if (!tag.empty()) {
                                    return std::unexpected(WireError::WrongType);
                                }
                                const auto row = detail::read_row<k_deriv_arity>(payload);
                                if (!row.has_value()) {
                                    return std::unexpected(row.error());
                                }
                                WireDerivStatus out{};
                                out.ts_ms = detail::digits_to_u64(row->at<k_deriv_mts>());
                                out.deriv_price = row->at<k_deriv_price>();
                                out.spot_price = row->at<k_deriv_spot_price>();
                                out.next_funding_ms =
                                    detail::digits_to_u64(row->at<k_deriv_next_funding_mts>());
                                out.next_funding_accrued = row->at<k_deriv_next_funding_accrued>();
                                out.current_funding = row->at<k_deriv_current_funding>();
                                out.mark_price = row->at<k_deriv_mark_price>();
                                out.open_interest = row->at<k_deriv_open_interest>();
                                return out;
                            });
    });
}

} // namespace raikiri::venues::bitfinex
