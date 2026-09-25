#include <raikiri/venues/apex/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::apex {

namespace {

using wire::guarded;
using wire::session;
namespace ondemand = wire::ondemand;

std::expected<WireContract, WireError> read_contract(ondemand::object& obj) {
    WireContract out{};
    const auto cross = wire::req_str(obj, "crossSymbolName");
    const auto base = wire::req_str(obj, "baseTokenId");
    const auto settle = wire::req_str(obj, "settleAssetId");
    const auto tick = wire::req_str(obj, "tickSize");
    const auto step = wire::req_str(obj, "stepSize");
    if (!cross.has_value() || !base.has_value() || !settle.has_value() || !tick.has_value() || !step.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.cross_symbol = *cross;
    out.base = *base;
    out.settle = *settle;
    out.tick_size = *tick;
    out.step_size = *step;
    out.funding_max_rate = wire::opt_str(obj, "fundingMaxRate").value_or(std::string_view{});
    out.funding_min_rate = wire::opt_str(obj, "fundingMinRate").value_or(std::string_view{});
    const auto trade = wire::opt_bool(obj, "enableTrade");
    if (!trade.has_value()) {
        return std::unexpected(trade.error());
    }
    out.enable_trade = *trade;
    return out;
}

std::expected<std::size_t, WireError>
read_contracts(ondemand::object& config, std::string_view key, ContractBuffer& storage, std::size_t count) {
    auto rows = wire::req_array(config, key);
    if (!rows.has_value()) {
        return std::unexpected(rows.error());
    }
    for (auto entry : *rows) {
        if (count == k_max_contracts) {
            return std::unexpected(WireError::TooManyEntries);
        }
        ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto contract = read_contract(obj);
        if (!contract.has_value()) {
            return std::unexpected(contract.error());
        }
        storage.contracts[count++] = *contract;
    }
    return count;
}

std::expected<std::size_t, WireError> read_levels(ondemand::array levels, std::span<WireLevel> out) {
    std::size_t count = 0;
    for (auto entry : levels) {
        if (count == out.size()) {
            return std::unexpected{WireError::TooManyEntries};
        }
        ondemand::array pair;
        if (entry.get_array().get(pair) != simdjson::SUCCESS) {
            return std::unexpected{WireError::WrongType};
        }
        std::size_t field = 0;
        WireLevel level{};
        for (auto item : pair) {
            ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected{text.error()};
            }
            if (field == 0) {
                level.price = *text;
            } else if (field == 1) {
                level.size = *text;
            }
            ++field;
        }
        if (field < 2) {
            return std::unexpected{WireError::MissingField};
        }
        out[count++] = level;
    }
    return count;
}

bool is_snapshot(ondemand::object& obj) {
    return wire::opt_str(obj, "type").value_or(std::string_view{}) == "snapshot";
}

} // namespace

std::expected<std::span<const WireContract>, WireError> parse_symbols(std::string_view body, ContractBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireContract>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = wire::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto config = wire::req_object(*data, "contractConfig");
        if (!config.has_value()) {
            return std::unexpected(config.error());
        }
        const auto perps = read_contracts(*config, "perpetualContract", storage, 0);
        if (!perps.has_value()) {
            return std::unexpected(perps.error());
        }
        const auto all = read_contracts(*config, "stockContract", storage, *perps);
        if (!all.has_value()) {
            return std::unexpected(all.error());
        }
        return std::span<const WireContract>{storage.contracts.data(), *all};
    });
}

TopicInfo classify_topic(std::string_view topic) noexcept {
    TopicInfo info{};
    const auto last_dot = topic.rfind('.');
    if (last_dot == std::string_view::npos) {
        return info;
    }
    if (topic.starts_with("orderBook")) {
        info.kind = TopicKind::Orderbook;
    } else if (topic.starts_with("recentlyTrade.")) {
        info.kind = TopicKind::Trade;
    } else if (topic.starts_with("instrumentInfo.")) {
        info.kind = TopicKind::Instrument;
    } else {
        return info;
    }
    info.symbol = topic.substr(last_dot + 1);
    return info;
}

std::expected<TopicInfo, WireError> parse_topic(std::string_view frame) {
    return guarded([&]() -> std::expected<TopicInfo, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        if (const auto topic = wire::opt_str(obj, "topic"); topic.has_value() && !topic->empty()) {
            return classify_topic(*topic);
        }
        if (wire::has_field(obj, "success")) {
            return TopicInfo{.kind = TopicKind::Ack, .symbol = {}};
        }
        if (wire::opt_str(obj, "op").value_or(std::string_view{}) == "ping") {
            return TopicInfo{.kind = TopicKind::Ping, .symbol = {}};
        }
        return std::unexpected{WireError::UnknownMessage};
    });
}

std::expected<WireAck, WireError> parse_ack(std::string_view frame, AckBuffer& storage) {
    return guarded([&]() -> std::expected<WireAck, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireAck out{};
        const auto success = wire::opt_bool(obj, "success");
        if (!success.has_value()) {
            return std::unexpected{success.error()};
        }
        out.success = *success;
        out.ret_msg = wire::opt_str(obj, "ret_msg").value_or(std::string_view{});
        if (!wire::has_field(obj, "request")) {
            return out;
        }
        auto request = wire::req_object(obj, "request");
        if (!request.has_value()) {
            return std::unexpected{request.error()};
        }
        out.op = wire::opt_str(*request, "op").value_or(std::string_view{});
        if (!wire::has_field(*request, "args")) {
            return out;
        }
        auto args = wire::req_array(*request, "args");
        if (!args.has_value()) {
            return std::unexpected{args.error()};
        }
        std::size_t count = 0;
        for (auto item : *args) {
            if (count == storage.args.size()) {
                return std::unexpected{WireError::TooManyEntries};
            }
            ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected{text.error()};
            }
            storage.args[count++] = *text;
        }
        out.args = {storage.args.data(), count};
        return out;
    });
}

std::expected<std::string_view, WireError> parse_ping(std::string_view frame) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto args = wire::req_array(*root, "args");
        if (!args.has_value()) {
            return std::unexpected{args.error()};
        }
        for (auto item : *args) {
            ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            return wire::str_of(value);
        }
        return std::unexpected{WireError::MissingField};
    });
}

std::expected<WireOrderbook, WireError> parse_orderbook(std::string_view frame, OrderbookBuffer& storage) {
    return guarded([&]() -> std::expected<WireOrderbook, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireOrderbook out{};
        out.is_snapshot = is_snapshot(obj);
        out.ts_us = wire::opt_u64(obj, "ts").value_or(0);
        auto data = wire::req_object(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }
        auto book = *data;
        const auto symbol = wire::req_str(book, "s");
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        out.symbol = *symbol;
        if (!wire::has_field(book, "u")) {
            return std::unexpected{WireError::MissingField};
        }
        const auto update_id = wire::opt_u64(book, "u");
        if (!update_id.has_value()) {
            return std::unexpected{update_id.error()};
        }
        out.update_id = *update_id;
        std::size_t bids = 0;
        if (auto side = wire::req_array(book, "b"); side.has_value()) {
            const auto n = read_levels(*side, storage.bids);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            bids = *n;
        }
        std::size_t asks = 0;
        if (auto side = wire::req_array(book, "a"); side.has_value()) {
            const auto n = read_levels(*side, storage.asks);
            if (!n.has_value()) {
                return std::unexpected{n.error()};
            }
            asks = *n;
        }
        out.bids = {storage.bids.data(), bids};
        out.asks = {storage.asks.data(), asks};
        return out;
    });
}

std::expected<WireTrades, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return guarded([&]() -> std::expected<WireTrades, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireTrades out{};
        out.is_snapshot = is_snapshot(obj);
        auto data = wire::req_array(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }
        std::size_t count = 0;
        for (auto entry : *data) {
            if (count == storage.trades.size()) {
                return std::unexpected{WireError::TooManyEntries};
            }
            ondemand::object row;
            if (entry.get_object().get(row) != simdjson::SUCCESS) {
                return std::unexpected{WireError::WrongType};
            }
            WireTrade trade{};
            const auto symbol = wire::req_str(row, "s");
            const auto price = wire::req_str(row, "p");
            const auto size = wire::req_str(row, "v");
            const auto side = wire::req_str(row, "S");
            if (!symbol.has_value() || !price.has_value() || !size.has_value() || !side.has_value()) {
                return std::unexpected{WireError::MissingField};
            }
            trade.symbol = *symbol;
            trade.price = *price;
            trade.size = *size;
            trade.buyer_aggressor = *side == "Buy";
            trade.ts_ms = wire::opt_u64(row, "T").value_or(0);
            storage.trades[count++] = trade;
        }
        out.trades = {storage.trades.data(), count};
        return out;
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected{root.error()};
        }
        auto obj = *root;
        WireTicker out{};
        out.ts_us = wire::opt_u64(obj, "ts").value_or(0);
        auto data = wire::req_object(obj, "data");
        if (!data.has_value()) {
            return std::unexpected{data.error()};
        }
        auto row = *data;
        const auto symbol = wire::req_str(row, "symbol");
        if (!symbol.has_value()) {
            return std::unexpected{symbol.error()};
        }
        out.symbol = *symbol;
        const auto field = [&](std::string_view key) { return wire::opt_str(row, key).value_or(std::string_view{}); };
        out.mark_price = field("markPrice");
        out.funding_rate = field("fundingRate");
        out.next_funding_time = field("nextFundingTime");
        out.volume_24h = field("volume24h");
        out.turnover_24h = field("turnover24h");
        return out;
    });
}

} // namespace raikiri::venues::apex
