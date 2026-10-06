#include <raikiri/venues/arcus/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::arcus {

namespace json = raikiri::wire;

namespace {

std::expected<WireMarket, WireError> read_market(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "marketDisplayName");
    const auto status = json::opt_str(row, "status");
    const auto base = json::req_str(row, "baseAsset");
    const auto quote = json::req_str(row, "quoteAsset");
    const auto tick = json::req_str(row, "tickSize");
    const auto step = json::req_str(row, "stepSize");
    const auto min = json::opt_str(row, "minOrderNotional");
    const auto type = json::req_str(row, "type");
    const auto category = json::opt_str(row, "category");
    if (!symbol.has_value() || !status.has_value() || !base.has_value() || !quote.has_value() || !tick.has_value() ||
        !step.has_value() || !min.has_value() || !type.has_value() || !category.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireMarket{.symbol = *symbol,
                      .base = *base,
                      .quote = *quote,
                      .type = *type,
                      .status = *status,
                      .category = *category,
                      .tick = *tick,
                      .step = *step,
                      .min_notional = *min};
}

std::expected<WireStats, WireError> read_stats(json::ondemand::object& row) {
    const auto symbol = json::req_str(row, "marketDisplayName");
    const auto oracle = json::opt_str(row, "oraclePrice");
    const auto mark = json::opt_str(row, "markPrice");
    const auto funding = json::opt_str(row, "fundingRate");
    const auto next = json::opt_str(row, "nextFundingRate");
    const auto next_at = json::opt_raw(row, "nextFundingAt");
    const auto volume = json::opt_str(row, "volume24hNotional");
    const auto oi = json::opt_str(row, "openInterest");
    if (!symbol.has_value() || !oracle.has_value() || !mark.has_value() || !funding.has_value() || !next.has_value() ||
        !next_at.has_value() || !volume.has_value() || !oi.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireStats{.symbol = *symbol,
                     .mark = *mark,
                     .oracle = *oracle,
                     .funding = *funding,
                     .next_funding = *next,
                     .next_funding_s = json::digits_to_u64(*next_at),
                     .volume_notional = *volume,
                     .open_interest = *oi};
}

std::expected<std::size_t, WireError> read_side(json::ondemand::array& side, std::span<WireLevel> into) {
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        json::ondemand::array pair;
        if (const auto ec = entry.get_array().get(pair); ec != simdjson::SUCCESS) {
            return std::unexpected(json::map_error(ec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t n = 0;
        for (auto cell : pair) {
            if (n == cells.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::value value;
            if (const auto ec = cell.get(value); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto text = json::str_of(value);
            if (!text.has_value() || text->empty()) {
                return std::unexpected(WireError::WrongType);
            }
            cells[n] = *text;
            ++n;
        }
        if (n != cells.size()) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

std::expected<WireLevel, WireError> read_quote(json::ondemand::object& contents, std::string_view key) {
    auto side = json::req_object(contents, key);
    if (!side.has_value()) {
        return std::unexpected(side.error());
    }
    const auto price = json::req_str(*side, "price");
    const auto qty = json::req_str(*side, "size");
    if (!price.has_value() || !qty.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    return WireLevel{.price = *price, .qty = *qty};
}

FrameType frame_type(std::string_view type) noexcept {
    if (type == "channel_data") {
        return FrameType::Data;
    }
    if (type == "subscribed") {
        return FrameType::Subscribed;
    }
    if (type == "unsubscribed") {
        return FrameType::Unsubscribed;
    }
    if (type == "degraded") {
        return FrameType::Degraded;
    }
    if (type == "error") {
        return FrameType::Error;
    }
    if (type == "connected") {
        return FrameType::Connected;
    }
    return FrameType::Unknown;
}

Channel channel_of(std::string_view channel) noexcept {
    if (channel == "bbo") {
        return Channel::Bbo;
    }
    if (channel == "l2OrderbookUpdates") {
        return Channel::Book;
    }
    if (channel == "trades") {
        return Channel::Trades;
    }
    if (channel == "markets") {
        return Channel::Markets;
    }
    return Channel::Unknown;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_markets(std::string_view body, MarketBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = json::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "markets");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.markets.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto one = read_market(row);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.markets[count] = *one;
            ++count;
        }
        return std::span<const WireMarket>{storage.markets.data(), count};
    });
}

std::expected<WireHead, WireError> parse_head(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireHead, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto type = json::req_str(*root, "type");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        const auto channel = json::opt_str(*root, "channel");
        const auto id = json::opt_str(*root, "id");
        const auto message = json::opt_str(*root, "message");
        if (!channel.has_value() || !id.has_value() || !message.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        return WireHead{.type = frame_type(*type), .channel = channel_of(*channel), .id = *id, .message = *message};
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return json::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = json::req_str(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto contents = json::req_object(*root, "contents");
        if (!contents.has_value()) {
            return std::unexpected(contents.error());
        }
        WireBook out{};
        out.id = *id;
        for (const bool bid : {true, false}) {
            auto side = json::req_array(*contents, bid ? "bids" : "asks");
            if (!side.has_value()) {
                return std::unexpected(side.error());
            }
            auto& into = bid ? storage.bids : storage.asks;
            const auto count = read_side(*side, into);
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            (bid ? out.bids : out.asks) = std::span<const WireLevel>{into.data(), *count};
        }
        const auto seq = json::opt_u64(*contents, "lastSequenceId");
        if (!seq.has_value() || *seq == 0) {
            return std::unexpected(WireError::MissingField);
        }
        out.seq = *seq;
        return out;
    });
}

std::expected<WireBbo, WireError> parse_bbo(std::string_view frame) {
    return json::guarded([&]() -> std::expected<WireBbo, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto id = json::req_str(*root, "id");
        if (!id.has_value()) {
            return std::unexpected(id.error());
        }
        auto contents = json::req_object(*root, "contents");
        if (!contents.has_value()) {
            return std::unexpected(contents.error());
        }
        const auto bid = read_quote(*contents, "bestBid");
        const auto ask = read_quote(*contents, "bestAsk");
        const auto seq = json::opt_u64(*contents, "lastSequenceId");
        const auto ts = json::opt_u64(*contents, "timestamp");
        if (!bid.has_value() || !ask.has_value() || !seq.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        return WireBbo{.id = *id,
                       .seq = *seq,
                       .ts_us = *ts,
                       .bid = bid->price,
                       .bid_qty = bid->qty,
                       .ask = ask->price,
                       .ask_qty = ask->qty};
    });
}

std::expected<std::span<const WirePrint>, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WirePrint>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto rows = json::req_array(*root, "contents");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        std::size_t count = 0;
        for (auto entry : *rows) {
            if (count == storage.prints.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            json::ondemand::object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(json::map_error(ec));
            }
            const auto symbol = json::req_str(row, "marketDisplayName");
            const auto side = json::req_str(row, "side");
            const auto price = json::req_str(row, "price");
            const auto qty = json::req_str(row, "size");
            const auto id = json::req_str(row, "tradeId");
            const auto ts = json::opt_u64(row, "timestamp");
            if (!symbol.has_value() || !side.has_value() || !price.has_value() || !qty.has_value() || !id.has_value() ||
                !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            const auto trade_id = json::digits_to_u64(*id);
            if (trade_id == 0) {
                return std::unexpected(WireError::WrongType);
            }
            storage.prints[count] = WirePrint{
                .symbol = *symbol, .side = *side, .price = *price, .qty = *qty, .trade_id = trade_id, .ts_us = *ts};
            ++count;
        }
        return std::span<const WirePrint>{storage.prints.data(), count};
    });
}

std::expected<std::span<const WireStats>, WireError> parse_stats(std::string_view frame, StatsBuffer& storage) {
    return json::guarded([&]() -> std::expected<std::span<const WireStats>, WireError> {
        auto root = json::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto contents = json::req_object(*root, "contents");
        if (!contents.has_value()) {
            return std::unexpected(contents.error());
        }
        auto markets = json::req_object(*contents, "markets");
        if (!markets.has_value()) {
            return std::unexpected(markets.error());
        }
        std::size_t count = 0;
        const auto done = json::for_each_field(
            *markets, [&](std::string_view, json::ondemand::value& value) -> std::expected<void, WireError> {
                if (count == storage.rows.size()) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                json::ondemand::object row;
                if (const auto ec = value.get_object().get(row); ec != simdjson::SUCCESS) {
                    return std::unexpected(json::map_error(ec));
                }
                const auto one = read_stats(row);
                if (!one.has_value()) {
                    return std::unexpected(one.error());
                }
                storage.rows[count] = *one;
                ++count;
                return {};
            });
        if (!done.has_value()) {
            return std::unexpected(done.error());
        }
        return std::span<const WireStats>{storage.rows.data(), count};
    });
}

} // namespace raikiri::venues::arcus
