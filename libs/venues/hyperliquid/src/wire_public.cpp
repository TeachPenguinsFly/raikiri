#include "wire_detail.hpp"

namespace raikiri::venues::hyperliquid {

namespace {

using detail::guarded;
using detail::session;

EventKind kind_from_channel(std::string_view channel) noexcept {
    if (channel == "bbo") {
        return EventKind::Bbo;
    }
    if (channel == "trades") {
        return EventKind::Trade;
    }
    if (channel == "l2Book") {
        return EventKind::Book;
    }
    if (channel == "activeAssetCtx") {
        return EventKind::AssetCtx;
    }
    if (channel == "subscriptionResponse") {
        return EventKind::SubscribeAck;
    }
    if (channel == "pong") {
        return EventKind::Pong;
    }
    if (channel == "error") {
        return EventKind::Error;
    }
    return EventKind::Unknown;
}

// {"px":"64143.0","sz":"26.35385","n":66}; nothing downstream models `n`.
std::expected<WireLevel, WireError> read_level(detail::ondemand::value entry) {
    detail::ondemand::object obj;
    if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
        return std::unexpected(detail::map_error(ec));
    }
    WireLevel level{};
    const auto price = detail::req_str(obj, "px");
    const auto qty = detail::req_str(obj, "sz");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    level.price = *price;
    level.qty = *qty;
    return level;
}

std::expected<std::size_t, WireError> read_levels(detail::ondemand::array levels,
                                                  std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : levels) {
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

std::expected<EventKind, WireError> parse_event(std::string_view frame) {
    return guarded([&]() -> std::expected<EventKind, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto channel = detail::req_str(*root, "channel");
        if (!channel.has_value()) {
            return std::unexpected(channel.error());
        }
        return kind_from_channel(*channel);
    });
}

std::expected<StreamError, WireError> parse_stream_error(std::string_view frame) {
    return guarded([&]() -> std::expected<StreamError, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        // `data` is prose, not an object: there is no code to read.
        const auto msg = detail::req_str(*root, "data");
        if (!msg.has_value()) {
            return std::unexpected(msg.error());
        }
        return StreamError{.msg = *msg};
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data_object(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireBbo, WireError> {
                WireBbo out{};
                const auto coin = detail::req_str(obj, "coin");
                const auto ts = detail::opt_ms(obj, "time");
                if (!coin.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.coin = *coin;
                out.ts_ms = *ts;
                auto pair = detail::req_array(obj, "bbo");
                if (!pair.has_value()) {
                    return std::unexpected(pair.error());
                }
                std::size_t index = 0;
                for (auto entry : *pair) {
                    detail::ondemand::value value;
                    if (const auto ec = entry.get(value); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    if (value.is_null()) {
                        ++index;
                        continue;
                    }
                    const auto level = read_level(value);
                    if (!level.has_value()) {
                        return std::unexpected(level.error());
                    }
                    if (index == 0) {
                        out.bid_price = level->price;
                        out.bid_qty = level->qty;
                    } else if (index == 1) {
                        out.ask_price = level->price;
                        out.ask_qty = level->qty;
                    }
                    ++index;
                }
                if (index != 2) {
                    return std::unexpected(WireError::MissingField);
                }
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
        // The one channel whose `data` is an array rather than an object.
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
            const auto coin = detail::req_str(obj, "coin");
            const auto price = detail::req_str(obj, "px");
            const auto qty = detail::req_str(obj, "sz");
            const auto side = detail::req_str(obj, "side");
            const auto trade_id = detail::opt_raw(obj, "tid");
            const auto ts = detail::opt_ms(obj, "time");
            if (!coin.has_value() || !price.has_value() || !qty.has_value() || !side.has_value() ||
                !trade_id.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            out.coin = *coin;
            out.price = *price;
            out.qty = *qty;
            out.trade_id = *trade_id;
            out.aggressor_bought = *side == "B";
            out.ts_ms = *ts;
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireAssetCtx, WireError> parse_asset_ctx(std::string_view frame) {
    return guarded([&]() -> std::expected<WireAssetCtx, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data_object(
            *root, [](detail::ondemand::object& obj) -> std::expected<WireAssetCtx, WireError> {
                WireAssetCtx out{};
                const auto coin = detail::req_str(obj, "coin");
                if (!coin.has_value()) {
                    return std::unexpected(coin.error());
                }
                out.coin = *coin;
                auto ctx = detail::req_object(obj, "ctx");
                if (!ctx.has_value()) {
                    return std::unexpected(ctx.error());
                }
                if (const auto ok = detail::read_ctx_fields(*ctx, out); !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                return out;
            });
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return detail::with_data_object(
            *root, [&](detail::ondemand::object& obj) -> std::expected<WireBook, WireError> {
                WireBook out{};
                const auto coin = detail::req_str(obj, "coin");
                const auto ts = detail::opt_ms(obj, "time");
                if (!coin.has_value() || !ts.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                out.coin = *coin;
                out.ts_ms = *ts;
                auto levels = detail::req_array(obj, "levels");
                if (!levels.has_value()) {
                    return std::unexpected(levels.error());
                }
                std::size_t side = 0;
                for (auto entry : *levels) {
                    detail::ondemand::array inner;
                    if (const auto ec = entry.get_array().get(inner); ec != simdjson::SUCCESS) {
                        return std::unexpected(detail::map_error(ec));
                    }
                    if (side > 1) {
                        return std::unexpected(WireError::TooManyEntries);
                    }
                    auto& into = side == 0 ? storage.bids : storage.asks;
                    const auto count = read_levels(inner, into);
                    if (!count.has_value()) {
                        return std::unexpected(count.error());
                    }
                    (side == 0 ? out.bids : out.asks) = {into.data(), *count};
                    ++side;
                }
                if (side != 2) {
                    return std::unexpected(WireError::MissingField);
                }
                return out;
            });
    });
}

} // namespace raikiri::venues::hyperliquid
