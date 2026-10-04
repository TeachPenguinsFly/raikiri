#include <raikiri/venues/kucoin/account.hpp>
#include <raikiri/venues/kucoin/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::kucoin {

namespace {

using wire::guarded;
using wire::session;

} // namespace

std::expected<WireAccountOverview, WireError> parse_account_overview(std::string_view body) {
    return guarded([&]() -> std::expected<WireAccountOverview, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root, [](wire::ondemand::object& obj) -> std::expected<WireAccountOverview, WireError> {
            const auto currency = req_decimal(obj, "currency");
            if (!currency.has_value()) {
                return std::unexpected(currency.error());
            }
            WireAccountOverview out{};
            out.currency = *currency;
            for (const auto& [key, field] : {std::pair{"marginBalance", &out.margin_balance},
                                             std::pair{"availableBalance", &out.available_balance},
                                             std::pair{"unrealisedPNL", &out.unrealised_pnl}}) {
                const auto value = opt_decimal(obj, key);
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                *field = *value;
            }
            return out;
        });
    });
}

std::expected<WireFundingPage, WireError> parse_funding_history(std::string_view body, FundingChargeBuffer& storage) {
    return guarded([&]() -> std::expected<WireFundingPage, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_data(*root, [&](wire::ondemand::object& obj) -> std::expected<WireFundingPage, WireError> {
            auto array = wire::req_array(obj, "dataList");
            if (!array.has_value()) {
                return std::unexpected(array.error());
            }
            const auto count = read_rows<WireFundingCharge>(
                *array, storage.charges, [](auto& row_obj, WireFundingCharge& row) -> std::expected<void, WireError> {
                    const auto id = opt_u64(row_obj, "id");
                    const auto symbol = req_decimal(row_obj, "symbol");
                    const auto time = opt_u64(row_obj, "timePoint");
                    const auto funding = req_decimal(row_obj, "funding");
                    const auto currency = opt_decimal(row_obj, "settleCurrency");
                    if (!id.has_value() || !symbol.has_value() || !time.has_value() || !funding.has_value() ||
                        !currency.has_value()) {
                        return std::unexpected(WireError::WrongType);
                    }
                    row.id = *id;
                    row.symbol = *symbol;
                    row.time_ms = *time;
                    row.funding = *funding;
                    row.settle_currency = *currency;
                    return {};
                });
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            const auto has_more = wire::opt_bool(obj, "hasMore");
            if (!has_more.has_value()) {
                return std::unexpected(has_more.error());
            }
            return WireFundingPage{.charges = {storage.charges.data(), *count}, .has_more = *has_more};
        });
    });
}

} // namespace raikiri::venues::kucoin
