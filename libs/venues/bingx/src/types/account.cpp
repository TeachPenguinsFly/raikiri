#include <raikiri/venues/bingx/account.hpp>
#include <raikiri/venues/bingx/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

#include <span>
#include <utility>

namespace raikiri::venues::bingx {

namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

} // namespace

std::expected<std::string_view, WireError> parse_listen_key(std::string_view body) {
    return guarded([&]() -> std::expected<std::string_view, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        const auto direct = detail::opt_str(*root, "listenKey");
        if (!direct.has_value()) {
            return std::unexpected(direct.error());
        }
        if (!direct->empty()) {
            return *direct;
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(WireError::MissingField);
        }
        const auto nested = detail::req_str(*data, "listenKey");
        if (!nested.has_value()) {
            return std::unexpected(nested.error());
        }
        return *nested;
    });
}

namespace {

// The rows under `data`, read into caller storage. Balances and income rows
// differ only in the per-row walk, and BingX puts both in a bare array.
template <typename Entry, typename Read>
std::expected<std::span<const Entry>, WireError>
read_data(std::string_view body, std::span<Entry> into, Read&& read) {
    auto root = session().open_object(body);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto array = detail::req_array(*root, "data");
    if (!array.has_value()) {
        return std::unexpected(array.error());
    }
    std::size_t count = 0;
    for (auto element : *array) {
        if (count == into.size()) {
            return std::unexpected(WireError::TooManyEntries);
        }
        detail::ondemand::object obj;
        if (const auto ec = element.get_object().get(obj); ec != simdjson::SUCCESS) {
            return std::unexpected(detail::map_error(ec));
        }
        Entry entry{};
        if (const auto ok = read(obj, entry); !ok.has_value()) {
            return std::unexpected(ok.error());
        }
        into[count] = entry;
        ++count;
    }
    return std::span<const Entry>{into.data(), count};
}

// Every field optional, the asset included, because this endpoint IS KNOWN TO
// SEND A ROW WITH NO ASSET: refusing the body over one would cost the account
// every balance beside it. The mapper drops the nameless row instead.
std::expected<void, WireError> read_balance(detail::ondemand::object& obj, WireBalance& out) {
    for (const auto& [key, field] : {std::pair{"asset", &out.asset},
                                     std::pair{"balance", &out.balance},
                                     std::pair{"availableMargin", &out.available_margin},
                                     std::pair{"unrealizedProfit", &out.unrealized_profit}}) {
        const auto value = opt_scalar(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    return {};
}

std::expected<void, WireError> read_income(detail::ondemand::object& obj, WireIncome& out) {
    for (const auto& [key, field] : {std::pair{"symbol", &out.symbol},
                                     std::pair{"incomeType", &out.income_type},
                                     std::pair{"income", &out.income},
                                     std::pair{"asset", &out.asset},
                                     std::pair{"tranId", &out.tran_id},
                                     std::pair{"tradeId", &out.trade_id}}) {
        const auto value = opt_scalar(obj, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    const auto at = opt_ms_any(obj, "time");
    if (!at.has_value()) {
        return std::unexpected(at.error());
    }
    out.time_ms = *at;
    return {};
}

} // namespace

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                      BalanceBuffer& storage) {
    return guarded([&] { return read_data<WireBalance>(body, storage.balances, read_balance); });
}

std::expected<std::span<const WireIncome>, WireError> parse_income(std::string_view body,
                                                                   IncomeBuffer& storage) {
    return guarded([&] { return read_data<WireIncome>(body, storage.rows, read_income); });
}

std::expected<bool, WireError> parse_position_mode(std::string_view body) {
    return guarded([&]() -> std::expected<bool, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto data = detail::req_object(*root, "data");
        if (!data.has_value()) {
            return std::unexpected(data.error());
        }
        const auto text = req_scalar(*data, "dualSidePosition");
        if (!text.has_value()) {
            return std::unexpected(text.error());
        }
        return *text == "true";
    });
}

} // namespace raikiri::venues::bingx
