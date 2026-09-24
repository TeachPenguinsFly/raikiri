#include <raikiri/venues/htx/envelope.hpp>
#include <raikiri/venues/htx/market_data.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::htx {

namespace {

using wire::guarded;
using wire::session;

// payload is under tick on the market socket and data[0] on the notification socket
std::expected<wire::ondemand::object, WireError> first_data(wire::ondemand::object& root) {
    auto array = wire::req_array(root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    for (auto entry : *array) {
        wire::ondemand::object obj;
        if (const auto ec = entry.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(wire::map_error(ec));
        }
        return obj;
    }
    return std::unexpected(WireError::MissingField);
}

} // namespace

std::expected<std::span<const WireTrade>, WireError> parse_trades(std::string_view frame,
                                                                  TradeBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireTrade>, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ch = wire::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        const auto code = code_of(*ch);
        auto tick = wire::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        auto array = wire::req_array(*tick, "data");
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
            out.contract_code = code;
            std::expected<std::string_view, WireError> amount{std::string_view{}};
            std::expected<std::string_view, WireError> quantity{std::string_view{}};
            std::expected<std::string_view, WireError> price{std::string_view{}};
            std::expected<std::uint64_t, WireError> ts{0};
            std::expected<std::uint64_t, WireError> id{0};
            std::expected<std::string_view, WireError> direction{
                std::unexpected(WireError::MissingField)};
            const auto walked = wire::for_each_field(
                obj, [&](std::string_view key, wire::ondemand::value& value) {
                    if (key == "amount") {
                        amount = wire::raw_of(value);
                    } else if (key == "quantity") {
                        quantity = wire::raw_of(value);
                    } else if (key == "price") {
                        price = wire::raw_of(value);
                    } else if (key == "ts") {
                        ts = wire::u64_of(value);
                    } else if (key == "id") {
                        id = wire::u64_of(value);
                    } else if (key == "direction") {
                        direction = wire::str_of(value);
                    }
                });
            if (!walked.has_value()) {
                return std::unexpected(walked.error());
            }
            if (!amount.has_value() || !quantity.has_value() || !ts.has_value() ||
                !id.has_value() || !price.has_value() || !direction.has_value() ||
                direction->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            if (amount->empty() || price->empty()) {
                return std::unexpected(WireError::MissingField);
            }
            out.amount = *amount;
            out.quantity = *quantity;
            out.price = *price;
            out.trade_id = *id;
            out.ts_ms = *ts;
            out.aggressor_bought = *direction == "buy";
            storage.trades[count] = out;
            ++count;
        }
        return std::span<const WireTrade>{storage.trades.data(), count};
    });
}

std::expected<WireDetail, WireError> parse_detail(std::string_view frame) {
    return guarded([&]() -> std::expected<WireDetail, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ch = wire::req_str(*root, "ch");
        if (!ch.has_value()) {
            return std::unexpected(ch.error());
        }
        const auto ts = wire::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        auto tick = wire::req_object(*root, "tick");
        if (!tick.has_value()) {
            return std::unexpected(tick.error());
        }
        WireDetail out{};
        out.contract_code = code_of(*ch);
        out.ts_ms = *ts;
        // both optional; empty is not zero
        const auto amount = wire::opt_raw(*tick, "amount");
        const auto turnover = wire::opt_raw(*tick, "trade_turnover");
        if (!amount.has_value() || !turnover.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.amount = *amount;
        out.trade_turnover = *turnover;
        return out;
    });
}

std::expected<WireFunding, WireError> parse_funding(std::string_view frame) {
    return guarded([&]() -> std::expected<WireFunding, WireError> {
        auto root = session().open_object(frame);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto ts = wire::opt_u64(*root, "ts");
        if (!ts.has_value()) {
            return std::unexpected(ts.error());
        }
        auto obj = first_data(*root);
        if (!obj.has_value()) {
            return std::unexpected(obj.error());
        }
        WireFunding out{};
        out.ts_ms = *ts;
        const auto code = wire::req_str(*obj, "contract_code");
        if (!code.has_value()) {
            return std::unexpected(code.error());
        }
        out.contract_code = *code;
        const auto funding_ms = opt_ms_str(*obj, "funding_time");
        const auto rate = wire::opt_str(*obj, "funding_rate");
        const auto estimated = wire::opt_str(*obj, "estimated_rate");
        const auto settlement_ms = opt_ms_str(*obj, "settlement_time");
        if (!funding_ms.has_value() || !rate.has_value() || !estimated.has_value() ||
            !settlement_ms.has_value()) {
            return std::unexpected(WireError::WrongType);
        }
        out.funding_ms = *funding_ms;
        out.funding_rate = *rate;
        out.estimated_rate = *estimated;
        out.settlement_ms = *settlement_ms;
        return out;
    });
}

} // namespace raikiri::venues::htx
