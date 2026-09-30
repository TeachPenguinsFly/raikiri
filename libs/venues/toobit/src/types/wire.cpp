#include <raikiri/venues/toobit/wire.hpp>
#include <raikiri/wire/json.hpp>

#include <charconv>

namespace raikiri::venues::toobit {

namespace {

using Object = wire::ondemand::object;
using Array = wire::ondemand::array;

// toobit sends a field as a string on rest and as a number on the socket.
std::expected<std::string_view, WireError> loose_text(Object& obj, std::string_view key) {
    wire::ondemand::value value;
    const auto ec = wire::find(obj, key).get(value);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::string_view{};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    const auto raw = wire::raw_of(value);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    const auto text = wire::trim(*raw);
    if (text == "null") {
        return std::string_view{};
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::expected<std::uint64_t, WireError> loose_u64(Object& obj, std::string_view key) {
    const auto text = loose_text(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return wire::digits_to_u64(*text);
}

std::expected<bool, WireError> loose_bool(Object& obj, std::string_view key) {
    const auto text = loose_text(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return *text == "true";
}

template <typename Row, typename Read>
std::expected<std::size_t, WireError> read_rows(Array& rows, std::span<Row> into, std::size_t count, Read&& read) {
    for (auto entry : rows) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        Object row;
        if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto one = read(row);
        if (!one.has_value()) {
            return std::unexpected(one.error());
        }
        into[count] = *one;
        ++count;
    }
    return count;
}

template <typename Row, typename Read>
std::expected<std::span<const Row>, WireError> parse_array(std::string_view body, std::span<Row> into, Read&& read) {
    return wire::guarded([&]() -> std::expected<std::span<const Row>, WireError> {
        auto root = wire::session().open_array(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto count = read_rows(*root, into, 0, read);
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const Row>{into.data(), *count};
    });
}

std::expected<void, WireError> read_filters(Object& row, WireMarket& out) {
    auto filters = wire::req_array(row, "filters");
    if (!filters.has_value()) {
        return std::unexpected(filters.error());
    }
    for (auto entry : *filters) {
        Object filter;
        if (const auto ec = entry.get_object().get(filter); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        const auto type = wire::opt_str(filter, "filterType");
        if (!type.has_value()) {
            return std::unexpected(type.error());
        }
        if (*type == "PRICE_FILTER") {
            const auto tick = wire::opt_str(filter, "tickSize");
            if (!tick.has_value()) {
                return std::unexpected(tick.error());
            }
            out.tick = *tick;
        } else if (*type == "LOT_SIZE") {
            const auto step = wire::opt_str(filter, "stepSize");
            if (!step.has_value()) {
                return std::unexpected(step.error());
            }
            out.step = *step;
        } else if (*type == "MIN_NOTIONAL") {
            const auto min = wire::opt_str(filter, "minNotional");
            if (!min.has_value()) {
                return std::unexpected(min.error());
            }
            out.min_notional = *min;
        }
    }
    return {};
}

std::expected<WireMarket, WireError> read_market(Object& row, bool contract) {
    WireMarket out{};
    out.contract = contract;
    if (const auto filters = read_filters(row, out); !filters.has_value()) {
        return std::unexpected(filters.error());
    }
    const auto symbol = wire::req_str(row, "symbol");
    const auto status = wire::opt_str(row, "status");
    const auto base = wire::opt_str(row, "baseAsset");
    const auto quote = wire::opt_str(row, "quoteAsset");
    if (!symbol.has_value() || !status.has_value() || !base.has_value() || !quote.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = *symbol;
    out.status = *status;
    out.base = *base;
    out.quote = *quote;
    if (contract) {
        const auto margin = wire::opt_str(row, "marginToken");
        const auto underlying = wire::opt_str(row, "underlying");
        const auto multiplier = wire::opt_str(row, "contractMultiplier");
        const auto inverse = wire::opt_bool(row, "inverse");
        if (!margin.has_value() || !underlying.has_value() || !multiplier.has_value() || !inverse.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.margin = *margin;
        out.underlying = *underlying;
        out.multiplier = *multiplier;
        out.inverse = *inverse;
    }
    return out;
}

std::expected<std::size_t, WireError> read_side(Object& data, std::string_view key, std::span<WireLevel> into) {
    wire::ondemand::array side;
    const auto ec = wire::find(data, key).get_array().get(side);
    if (ec == simdjson::NO_SUCH_FIELD) {
        return std::size_t{0};
    }
    if (ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    std::size_t count = 0;
    for (auto entry : side) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        wire::ondemand::array pair;
        if (const auto pec = entry.get_array().get(pair); pec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(pec));
        }
        std::array<std::string_view, 2> cells{};
        std::size_t cell = 0;
        for (auto item : pair) {
            wire::ondemand::value value;
            if (item.get(value) != simdjson::SUCCESS || cell == cells.size()) {
                return std::unexpected(WireError::WrongType);
            }
            const auto text = wire::str_of(value);
            if (!text.has_value()) {
                return std::unexpected(text.error());
            }
            cells[cell] = *text;
            ++cell;
        }
        if (cell != cells.size()) {
            return std::unexpected(WireError::ShortArray);
        }
        into[count] = WireLevel{.price = cells[0], .qty = cells[1]};
        ++count;
    }
    return count;
}

// data is an object, an array of one, or an empty array on a subscribe ack.
std::expected<std::optional<Object>, WireError> data_object(Object& root) {
    wire::ondemand::value data;
    if (const auto ec = wire::find(root, "data").get(data); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    wire::ondemand::json_type type{};
    if (const auto ec = data.type().get(type); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    if (type == wire::ondemand::json_type::object) {
        Object out;
        if (const auto ec = data.get_object().get(out); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return std::optional<Object>{out};
    }
    if (type != wire::ondemand::json_type::array) {
        return std::unexpected(WireError::WrongType);
    }
    Array rows;
    if (const auto ec = data.get_array().get(rows); ec != simdjson::SUCCESS) {
        return std::unexpected(wire::map_error(ec));
    }
    for (auto entry : rows) {
        Object out;
        if (const auto ec = entry.get_object().get(out); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return std::optional<Object>{out};
    }
    return std::optional<Object>{};
}

std::expected<WireOrder, WireError> read_order(Object& row) {
    WireOrder out{};
    const auto symbol = wire::req_str(row, "symbol");
    const auto order_id = loose_text(row, "orderId");
    const auto client = loose_text(row, "clientOrderId");
    const auto price = loose_text(row, "price");
    const auto orig = loose_text(row, "origQty");
    const auto executed = loose_text(row, "executedQty");
    const auto type = wire::opt_str(row, "type");
    const auto side = wire::opt_str(row, "side");
    const auto tif = wire::opt_str(row, "timeInForce");
    const auto status = wire::opt_str(row, "status");
    const auto time = loose_u64(row, "time");
    const auto update = loose_u64(row, "updateTime");
    if (!symbol.has_value() || !order_id.has_value() || !client.has_value() || !price.has_value() ||
        !orig.has_value() || !executed.has_value() || !type.has_value() || !side.has_value() || !tif.has_value() ||
        !status.has_value() || !time.has_value() || !update.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = *symbol;
    out.order_id = *order_id;
    out.client_order_id = *client;
    out.price = *price;
    out.orig_qty = *orig;
    out.executed_qty = *executed;
    out.type = *type;
    out.side = *side;
    out.tif = *tif;
    out.status = *status;
    out.time_ms = *time;
    out.update_ms = *update;
    return out;
}

std::expected<WireExecution, WireError> read_execution(Object& row) {
    WireExecution out{};
    const auto symbol = wire::req_str(row, "s");
    const auto client = loose_text(row, "c");
    const auto order_id = loose_text(row, "i");
    const auto side = wire::opt_str(row, "S");
    const auto type = wire::opt_str(row, "o");
    const auto tif = wire::opt_str(row, "f");
    const auto status = wire::opt_str(row, "X");
    const auto qty = loose_text(row, "q");
    const auto price = loose_text(row, "p");
    const auto last_qty = loose_text(row, "l");
    const auto cum_qty = loose_text(row, "z");
    const auto last_price = loose_text(row, "L");
    const auto fee = loose_text(row, "n");
    const auto maker = loose_bool(row, "m");
    const auto close = loose_bool(row, "C");
    const auto event = loose_u64(row, "E");
    const auto update = loose_u64(row, "U");
    if (!symbol.has_value() || !client.has_value() || !order_id.has_value() || !side.has_value() || !type.has_value() ||
        !tif.has_value() || !status.has_value() || !qty.has_value() || !price.has_value() || !last_qty.has_value() ||
        !cum_qty.has_value() || !last_price.has_value() || !fee.has_value() || !maker.has_value() ||
        !close.has_value() || !event.has_value() || !update.has_value()) {
        return std::unexpected(WireError::MissingField);
    }
    out.symbol = *symbol;
    out.client_order_id = *client;
    out.order_id = *order_id;
    out.side = *side;
    out.order_type = *type;
    out.tif = *tif;
    out.status = *status;
    out.qty = *qty;
    out.price = *price;
    out.last_qty = *last_qty;
    out.cum_qty = *cum_qty;
    out.last_price = *last_price;
    out.fee = *fee;
    out.maker = *maker;
    out.close = *close;
    out.event_ms = *event;
    out.update_ms = *update;
    return out;
}

} // namespace

std::expected<std::span<const WireMarket>, WireError> parse_exchange_info(std::string_view body,
                                                                          MarketBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::span<const WireMarket>, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const std::span<WireMarket> into{storage.rows};
        auto spot = wire::req_array(*root, "symbols");
        if (!spot.has_value()) {
            return std::unexpected(spot.error());
        }
        const auto spot_count = read_rows(*spot, into, 0, [](Object& row) { return read_market(row, false); });
        if (!spot_count.has_value()) {
            return std::unexpected(spot_count.error());
        }
        auto perp = wire::req_array(*root, "contracts");
        if (!perp.has_value()) {
            return std::unexpected(perp.error());
        }
        const auto count = read_rows(*perp, into, *spot_count, [](Object& row) { return read_market(row, true); });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        return std::span<const WireMarket>{storage.rows.data(), *count};
    });
}

std::expected<std::span<const WireFunding>, WireError> parse_funding_rates(std::string_view body,
                                                                           FundingBuffer& storage) {
    return parse_array(
        body, std::span<WireFunding>{storage.rows}, [](Object& row) -> std::expected<WireFunding, WireError> {
            const auto symbol = wire::req_str(row, "symbol");
            const auto rate = wire::opt_str(row, "rate");
            const auto period = wire::opt_str(row, "period");
            const auto cap = wire::opt_str(row, "fundingRateCap");
            const auto floor = wire::opt_str(row, "fundingRateFloor");
            const auto next = loose_u64(row, "nextFundingTime");
            if (!symbol.has_value() || !rate.has_value() || !period.has_value() || !cap.has_value() ||
                !floor.has_value() || !next.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            WireFunding out{};
            out.symbol = *symbol;
            out.rate = *rate;
            out.period = *period;
            out.cap = *cap;
            out.floor = *floor;
            out.next_ms = *next;
            return out;
        });
}

std::expected<std::span<const WireTicker>, WireError> parse_tickers(std::string_view body, TickerBuffer& storage) {
    return parse_array(
        body, std::span<WireTicker>{storage.rows}, [](Object& row) -> std::expected<WireTicker, WireError> {
            const auto symbol = wire::req_str(row, "s");
            const auto volume = wire::opt_str(row, "v");
            const auto quote = wire::opt_str(row, "qv");
            const auto ts = loose_u64(row, "t");
            if (!symbol.has_value() || !volume.has_value() || !quote.has_value() || !ts.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            WireTicker out{};
            out.symbol = *symbol;
            out.volume = *volume;
            out.quote_volume = *quote;
            out.ts_ms = *ts;
            return out;
        });
}

std::expected<WireHead, WireError> parse_head(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<WireHead, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireHead out{};
        const auto topic = wire::opt_str(*root, "topic");
        if (!topic.has_value()) {
            return std::unexpected(topic.error());
        }
        if (topic->empty()) {
            if (wire::has_field(*root, "pong")) {
                out.channel = Channel::Pong;
                return out;
            }
            const auto code = loose_text(*root, "code");
            if (code.has_value() && !code->empty()) {
                out.channel = Channel::Error;
                out.code = *code;
            }
            return out;
        }
        if (*topic == "bookTicker") {
            out.channel = Channel::Bbo;
        } else if (*topic == "diffDepth") {
            out.channel = Channel::Depth;
        } else if (*topic == "trade") {
            out.channel = Channel::Trade;
        } else if (*topic == "markPrice") {
            out.channel = Channel::Mark;
        }
        const auto symbol = wire::opt_str(*root, "symbol");
        const auto first = wire::opt_bool(*root, "f");
        const auto send = wire::opt_u64(*root, "sendTime");
        if (!symbol.has_value() || !first.has_value() || !send.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.first = *first;
        out.send_ms = *send;
        return out;
    });
}

std::expected<std::optional<WireBbo>, WireError> parse_bbo(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<std::optional<WireBbo>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_object(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        if (!data->has_value()) {
            return std::optional<WireBbo>{};
        }
        auto& row = **data;
        const auto symbol = wire::req_str(row, "s");
        const auto bid = wire::req_str(row, "b");
        const auto bid_qty = wire::req_str(row, "bq");
        const auto ask = wire::req_str(row, "a");
        const auto ask_qty = wire::req_str(row, "aq");
        const auto ts = wire::opt_u64(row, "t");
        if (!symbol.has_value() || !bid.has_value() || !bid_qty.has_value() || !ask.has_value() ||
            !ask_qty.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireBbo out{};
        out.symbol = *symbol;
        out.bid = *bid;
        out.bid_qty = *bid_qty;
        out.ask = *ask;
        out.ask_qty = *ask_qty;
        out.ts_ms = *ts;
        return std::optional<WireBbo>{out};
    });
}

std::expected<std::optional<WireDepth>, WireError> parse_depth(std::string_view frame, LevelBuffer& storage) {
    return wire::guarded([&]() -> std::expected<std::optional<WireDepth>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WireDepth out{};
        const auto symbol = wire::req_str(*root, "symbol");
        const auto first = wire::opt_bool(*root, "f");
        if (!symbol.has_value() || !first.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.first = *first;
        auto data = data_object(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        if (!data->has_value()) {
            return std::optional<WireDepth>{};
        }
        auto& row = **data;
        const auto ts = wire::opt_u64(row, "t");
        const auto bids = read_side(row, "b", storage.bids);
        const auto asks = read_side(row, "a", storage.asks);
        if (!ts.has_value() || !bids.has_value() || !asks.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.ts_ms = *ts;
        out.bids = std::span<const WireLevel>{storage.bids.data(), *bids};
        out.asks = std::span<const WireLevel>{storage.asks.data(), *asks};
        return std::optional<WireDepth>{out};
    });
}

std::expected<WirePrints, WireError> parse_trades(std::string_view frame, PrintBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WirePrints, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        WirePrints out{};
        const auto symbol = wire::req_str(*root, "symbol");
        const auto first = wire::opt_bool(*root, "f");
        if (!symbol.has_value() || !first.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        out.symbol = *symbol;
        out.first = *first;
        auto rows = wire::req_array(*root, "data");
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        const auto count = read_rows(
            *rows, std::span<WirePrint>{storage.rows}, 0, [](Object& row) -> std::expected<WirePrint, WireError> {
                const auto id = wire::opt_str(row, "v");
                const auto ts = wire::opt_u64(row, "t");
                const auto price = wire::req_str(row, "p");
                const auto qty = wire::req_str(row, "q");
                const auto maker = wire::opt_bool(row, "m");
                if (!id.has_value() || !ts.has_value() || !price.has_value() || !qty.has_value() ||
                    !maker.has_value()) {
                    return std::unexpected(WireError::MissingField);
                }
                WirePrint print{};
                print.id = wire::digits_to_u64(*id);
                print.ts_ms = *ts;
                print.price = *price;
                print.qty = *qty;
                print.buyer_maker = *maker;
                return print;
            });
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        out.rows = std::span<const WirePrint>{storage.rows.data(), *count};
        return out;
    });
}

std::expected<std::optional<WireMark>, WireError> parse_mark(std::string_view frame) {
    return wire::guarded([&]() -> std::expected<std::optional<WireMark>, WireError> {
        auto root = wire::session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = data_object(*root);
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        if (!data->has_value()) {
            return std::optional<WireMark>{};
        }
        auto& row = **data;
        const auto symbol = wire::req_str(row, "symbolId");
        const auto price = wire::req_str(row, "price");
        const auto ts = wire::opt_u64(row, "time");
        if (!symbol.has_value() || !price.has_value() || !ts.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        WireMark out{};
        out.symbol = *symbol;
        out.price = *price;
        out.ts_ms = *ts;
        return std::optional<WireMark>{out};
    });
}

std::optional<WireRestError> parse_rest_error(std::string_view body) {
    const auto parsed = wire::guarded([&]() -> std::expected<WireRestError, WireError> {
        WireRestError out{};
        const auto trimmed = wire::trim(body);
        if (trimmed.empty() || trimmed.front() != '{') {
            return out;
        }
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto code = loose_text(*root, "code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        if (code->empty()) {
            return out;
        }
        std::int64_t value = 0;
        const auto* const end = code->data() + code->size();
        if (const auto [stop, ec] = std::from_chars(code->data(), end, value); ec != std::errc{} || stop != end) {
            return std::unexpected(WireError::WrongType);
        }
        out.code = value == 200 ? 0 : value;
        const auto msg = loose_text(*root, "msg");
        out.msg = msg.value_or(std::string_view{});
        return out;
    });
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return *parsed;
}

std::expected<WireOrder, WireError> parse_order(std::string_view body) {
    return wire::guarded([&]() -> std::expected<WireOrder, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return read_order(*root);
    });
}

std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body, OrderBuffer& storage) {
    return parse_array(body, std::span<WireOrder>{storage.rows}, read_order);
}

std::expected<std::span<const WirePosition>, WireError> parse_positions(std::string_view body,
                                                                        PositionBuffer& storage) {
    return parse_array(
        body, std::span<WirePosition>{storage.rows}, [](Object& row) -> std::expected<WirePosition, WireError> {
            const auto symbol = wire::req_str(row, "symbol");
            const auto side = wire::req_str(row, "side");
            const auto avg = loose_text(row, "avgPrice");
            const auto position = loose_text(row, "position");
            if (!symbol.has_value() || !side.has_value() || !avg.has_value() || !position.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            WirePosition out{};
            out.symbol = *symbol;
            out.side = *side;
            out.avg_price = *avg;
            out.position = *position;
            return out;
        });
}

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body, BalanceBuffer& storage) {
    return parse_array(
        body, std::span<WireBalance>{storage.rows}, [](Object& row) -> std::expected<WireBalance, WireError> {
            const auto coin = wire::req_str(row, "coin");
            const auto balance = loose_text(row, "balance");
            const auto available = loose_text(row, "availableBalance");
            const auto unrealized = loose_text(row, "crossUnRealizedPnl");
            if (!coin.has_value() || !balance.has_value() || !available.has_value() || !unrealized.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            WireBalance out{};
            out.coin = *coin;
            out.balance = *balance;
            out.available = *available;
            out.unrealized = *unrealized;
            return out;
        });
}

std::expected<std::span<const WireUserTrade>, WireError> parse_user_trades(std::string_view body,
                                                                           UserTradeBuffer& storage) {
    return parse_array(
        body, std::span<WireUserTrade>{storage.rows}, [](Object& row) -> std::expected<WireUserTrade, WireError> {
            const auto id = loose_text(row, "id");
            const auto order_id = loose_text(row, "orderId");
            const auto symbol = wire::req_str(row, "symbol");
            const auto price = loose_text(row, "price");
            const auto qty = loose_text(row, "qty");
            const auto commission = loose_text(row, "commission");
            const auto side = wire::opt_str(row, "side");
            const auto maker = loose_bool(row, "isMaker");
            const auto time = loose_u64(row, "time");
            if (!id.has_value() || !order_id.has_value() || !symbol.has_value() || !price.has_value() ||
                !qty.has_value() || !commission.has_value() || !side.has_value() || !maker.has_value() ||
                !time.has_value()) {
                return std::unexpected(WireError::MissingField);
            }
            WireUserTrade out{};
            out.id = *id;
            out.order_id = *order_id;
            out.symbol = *symbol;
            out.price = *price;
            out.qty = *qty;
            out.commission = *commission;
            out.side = *side;
            out.maker = *maker;
            out.time_ms = *time;
            return out;
        });
}

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return wire::guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = wire::session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return wire::req_str(*root, "listenKey");
    });
}

std::expected<WirePrivate, WireError> parse_private(std::string_view frame, ExecutionBuffer& storage) {
    return wire::guarded([&]() -> std::expected<WirePrivate, WireError> {
        WirePrivate out{};
        const auto trimmed = wire::trim(frame);
        if (!trimmed.empty() && trimmed.front() == '{') {
            auto root = wire::session().open_object(frame);
            if (!root.has_value()) {
                return std::unexpected(root.error());
            }
            if (wire::has_field(*root, "pong")) {
                out.kind = PrivateKind::Pong;
                return out;
            }
            const auto type = wire::opt_str(*root, "eventType");
            if (type.has_value() && *type == "listenKeyWillExpire") {
                out.kind = PrivateKind::ListenKeyExpiring;
            }
            return out;
        }
        auto rows = wire::session().open_array(frame);
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        out.kind = PrivateKind::Events;
        std::size_t count = 0;
        for (auto entry : *rows) {
            Object row;
            if (const auto ec = entry.get_object().get(row); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            const auto event = wire::opt_str(row, "e");
            if (!event.has_value()) {
                return std::unexpected(event.error());
            }
            if (*event != "contractExecutionReport") {
                continue;
            }
            if (count == storage.rows.size()) {
                return std::unexpected(WireError::TooManyEntries);
            }
            const auto one = read_execution(row);
            if (!one.has_value()) {
                return std::unexpected(one.error());
            }
            storage.rows[count] = *one;
            ++count;
        }
        out.executions = std::span<const WireExecution>{storage.rows.data(), count};
        return out;
    });
}

} // namespace raikiri::venues::toobit
