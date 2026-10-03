#include <raikiri/venues/derive/wire.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::derive {

namespace {

std::expected<wire::ondemand::object, WireError> push_data(wire::ondemand::object& root) {
    auto params = wire::req_object(root, "params");
    if (!params.has_value()) {
        return std::unexpected(params.error());
    }
    return wire::req_object(*params, "data");
}

// a null object reads as absent
std::optional<wire::ondemand::object> nested(wire::ondemand::object& obj, std::string_view key) {
    wire::ondemand::object out;
    if (wire::find(obj, key).get_object().get(out) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    return out;
}

std::string_view text(wire::ondemand::object& obj, std::string_view key) {
    return wire::opt_str(obj, key).value_or(std::string_view{});
}

std::expected<std::size_t, WireError>
read_levels(wire::ondemand::object& root, std::string_view key, std::span<WireLevel> into) {
    auto array = wire::req_array(root, key);
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto entry : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
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
            const auto token = wire::str_of(value);
            if (!token.has_value()) {
                return std::unexpected(token.error());
            }
            (field == 0 ? level.price : level.qty) = *token;
            if (++field == 2) {
                break;
            }
        }
        if (field < 2) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = level;
        ++count;
    }
    return count;
}

} // namespace

std::expected<InstrumentPage, WireError> parse_instruments(std::string_view body, InstrumentBuffer& storage) {
    return wire::guarded([&]() -> std::expected<InstrumentPage, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto array = wire::req_array(*result, "instruments");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireInstrument row{};
            row.instrument_type = text(obj, "instrument_type");
            row.instrument_name = text(obj, "instrument_name");
            row.activation_s = wire::opt_u64(obj, "scheduled_activation").value_or(0);
            row.is_active = wire::opt_bool(obj, "is_active").value_or(false);
            row.tick_size = text(obj, "tick_size");
            row.minimum_amount = text(obj, "minimum_amount");
            row.amount_step = text(obj, "amount_step");
            row.maker_fee_rate = text(obj, "maker_fee_rate");
            row.taker_fee_rate = text(obj, "taker_fee_rate");
            row.base_currency = text(obj, "base_currency");
            row.quote_currency = text(obj, "quote_currency");
            if (auto details = nested(obj, "option_details"); details.has_value()) {
                row.expiry_s = wire::opt_u64(*details, "expiry").value_or(0);
                row.strike = text(*details, "strike");
                row.option_type = text(*details, "option_type");
            }
            storage.rows[count] = row;
            ++count;
        }
        InstrumentPage page{.rows = std::span<const WireInstrument>{storage.rows.data(), count}};
        if (auto pagination = nested(*result, "pagination"); pagination.has_value()) {
            page.num_pages = static_cast<std::uint32_t>(wire::opt_u64(*pagination, "num_pages").value_or(0));
        }
        return page;
    });
}

std::optional<WireRpcError> parse_rpc_error(std::string_view body) {
    using Found = std::optional<WireRpcError>;
    const auto found = wire::guarded([&]() -> std::expected<Found, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto error = nested(*root, "error");
        if (!error.has_value()) {
            return Found{};
        }
        return WireRpcError{.code = wire::opt_i64(*error, "code").value_or(0), .message = text(*error, "message")};
    });
    return found.value_or(Found{});
}

Head parse_head(std::string_view frame) {
    const auto head = wire::guarded([&]() -> std::expected<Head, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        if (text(*root, "method") != "subscription") {
            return wire::has_field(*root, "id") ? Head{.channel = Channel::Response} : Head{};
        }
        auto params = wire::req_object(*root, "params");
        if (!params.has_value()) {
            return Head{};
        }
        const auto name = text(*params, "channel");
        if (name.starts_with("orderbook.")) {
            return Head{.channel = Channel::Book, .name = name};
        }
        if (name.starts_with("ticker_slim.")) {
            return Head{.channel = Channel::Ticker, .name = name};
        }
        if (name.starts_with("trades.")) {
            return Head{.channel = Channel::Trades, .name = name};
        }
        return Head{.channel = Channel::Unknown, .name = name};
    });
    return head.value_or(Head{});
}

std::string_view instrument_of(std::string_view channel, std::size_t trailing_params) noexcept {
    const auto dot = channel.find('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    auto rest = channel.substr(dot + 1);
    for (std::size_t i = 0; i < trailing_params; ++i) {
        const auto last = rest.rfind('.');
        if (last == std::string_view::npos) {
            return {};
        }
        rest = rest.substr(0, last);
    }
    return rest;
}

std::expected<WireSubscribeAck, WireError> parse_subscribe_ack(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireSubscribeAck, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto result = wire::req_object(*root, "result");
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        auto status = wire::req_object(*result, "status");
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
        WireSubscribeAck ack{};
        const auto walked = wire::for_each_field(*status, [&](std::string_view, wire::ondemand::value value) {
            const auto verdict = wire::str_of(value);
            ++(verdict.has_value() && *verdict == "ok" ? ack.accepted : ack.refused);
        });
        if (!walked.has_value()) {
            return std::unexpected(walked.error());
        }
        return ack;
    });
}

std::expected<WireBook, WireError> parse_book(std::string_view frame, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WireBook, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        WireBook out{};
        const auto ts = wire::opt_u64(*data, "timestamp");
        const auto instrument = wire::req_str(*data, "instrument_name");
        const auto publish_id = wire::opt_u64(*data, "publish_id");
        if (!ts.has_value() || !instrument.has_value() || !publish_id.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.ts_ms = *ts;
        out.instrument = *instrument;
        out.publish_id = *publish_id;
        const auto bids = read_levels(*data, "bids", storage.bids);
        if (!bids.has_value()) {
            return std::unexpected(bids.error());
        }
        const auto asks = read_levels(*data, "asks", storage.asks);
        if (!asks.has_value()) {
            return std::unexpected(asks.error());
        }
        out.bids = std::span<const WireLevel>{storage.bids.data(), *bids};
        out.asks = std::span<const WireLevel>{storage.asks.data(), *asks};
        return out;
    });
}

std::expected<WireTicker, WireError> parse_ticker(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireTicker, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = push_data(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        auto ticker = wire::req_object(*data, "instrument_ticker");
        if (!ticker.has_value()) {
            return std::unexpected(ticker.error());
        }
        WireTicker out{};
        const auto ts = wire::opt_u64(*ticker, "t");
        if (!ts.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.ts_ms = *ts;
        out.best_ask_qty = text(*ticker, "A");
        out.best_ask = text(*ticker, "a");
        out.best_bid_qty = text(*ticker, "B");
        out.best_bid = text(*ticker, "b");
        out.funding = text(*ticker, "f");
        if (auto pricing = nested(*ticker, "option_pricing"); pricing.has_value()) {
            out.priced = true;
            out.delta = text(*pricing, "d");
            out.theta = text(*pricing, "t");
            out.gamma = text(*pricing, "g");
            out.vega = text(*pricing, "v");
            out.mark_iv = text(*pricing, "i");
            out.rho = text(*pricing, "r");
            out.forward = text(*pricing, "f");
            out.bid_iv = text(*pricing, "bi");
            out.ask_iv = text(*pricing, "ai");
        }
        out.index = text(*ticker, "I");
        out.mark = text(*ticker, "M");
        if (auto stats = nested(*ticker, "stats"); stats.has_value()) {
            out.contracts_24h = text(*stats, "c");
            out.notional_24h = text(*stats, "v");
            out.premium_24h = text(*stats, "pr");
        }
        if (out.mark.empty()) {
            return std::unexpected(WireError::MissingField);
        }
        return out;
    });
}

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame, TradeBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto params = wire::req_object(*root, "params");
        if (!params.has_value()) {
            return std::unexpected(params.error());
        }
        auto array = wire::req_array(*params, "data");
        if (!array.has_value()) {
            return std::unexpected(array.error());
        }
        std::size_t count = 0;
        for (auto entry : *array) {
            if (count == k_max_trades) {
                return std::unexpected(WireError::TooManyEntries);
            }
            wire::ondemand::object obj;
            if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            WireTrade out{};
            out.trade_id = text(obj, "trade_id");
            out.instrument = text(obj, "instrument_name");
            const auto ts = wire::opt_u64(obj, "timestamp");
            out.price = text(obj, "trade_price");
            out.qty = text(obj, "trade_amount");
            const auto direction = text(obj, "direction");
            if (!ts.has_value() || out.instrument.empty() || out.price.empty() || out.qty.empty() ||
                (direction != "buy" && direction != "sell")) {
                return std::unexpected(WireError::MissingField);
            }
            out.ts_ms = *ts;
            out.aggressor_bought = direction == "buy";
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

} // namespace raikiri::venues::derive
