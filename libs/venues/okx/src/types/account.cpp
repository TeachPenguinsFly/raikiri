#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/envelope.hpp>
#include <raikiri/wire/json.hpp>

#include <simdjson.h>

namespace raikiri::venues::okx {

namespace {

using wire::guarded;
using wire::session;

std::expected<void, WireError> read_bill(wire::ondemand::object& obj, WireBill& out) {
    // instId is optional: transfer and interest rows name no instrument
    if (const auto ok = read_strings(obj,
                                     {{"billId", &out.bill_id},
                                      {"instId", &out.inst_id},
                                      {"type", &out.type},
                                      {"balChg", &out.bal_chg},
                                      {"fee", &out.fee},
                                      {"tradeId", &out.trade_id}});
        !ok.has_value()) {
        return std::unexpected(ok.error());
    }
    const auto ts_ms = wire::opt_ms(obj, "ts");
    if (!ts_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.ts_ms = *ts_ms;
    return {};
}

} // namespace

std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body, BillBuffer& storage) {
    return guarded([&] { return read_data<WireBill>(body, storage.bills, read_bill); });
}

std::expected<AccountMode, WireError> parse_account_mode(std::string_view body) {
    return guarded([&]() -> std::expected<AccountMode, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        return with_first_data(*root, [](wire::ondemand::object& obj) {
            return wire::req_str(obj, "acctLv").transform([](std::string_view level) {
                // an unknown level stays Unknown rather than guessing the nearest mode
                if (level == "1") {
                    return AccountMode::Spot;
                }
                if (level == "2") {
                    return AccountMode::Futures;
                }
                if (level == "3") {
                    return AccountMode::MultiCurrency;
                }
                if (level == "4") {
                    return AccountMode::Portfolio;
                }
                return AccountMode::Unknown;
            });
        });
    });
}

std::expected<std::span<const WireCurrencyBalance>, WireError> parse_balances(std::string_view body,
                                                                              BalanceBuffer& storage) {
    return guarded([&]() -> std::expected<std::span<const WireCurrencyBalance>, WireError> {
        auto root = session().open_object(body);
        if (!root.has_value()) {
            return std::unexpected(root.error());
        }
        auto accounts = wire::req_array(*root, "data");
        if (!accounts.has_value()) {
            return std::unexpected(accounts.error());
        }
        std::size_t count = 0;
        for (auto element : *accounts) {
            wire::ondemand::object account;
            if (const auto ec = element.get_object().get(account); ec != simdjson::SUCCESS) {
                return std::unexpected(wire::map_error(ec));
            }
            auto details = wire::req_array(account, "details");
            if (!details.has_value()) {
                return std::unexpected(details.error());
            }
            for (auto row : *details) {
                if (count == k_max_currencies) {
                    return std::unexpected(WireError::TooManyEntries);
                }
                wire::ondemand::object obj;
                if (const auto ec = row.get_object().get(obj); ec != simdjson::SUCCESS) {
                    return std::unexpected(wire::map_error(ec));
                }
                WireCurrencyBalance out{};
                if (const auto ok = read_strings(obj,
                                                 {{"ccy", &out.ccy},
                                                  {"cashBal", &out.cash_bal},
                                                  {"availBal", &out.avail_bal},
                                                  {"upl", &out.upl}});
                    !ok.has_value()) {
                    return std::unexpected(ok.error());
                }
                const auto at = wire::opt_ms(obj, "uTime");
                if (!at.has_value()) {
                    return std::unexpected(at.error());
                }
                out.u_time_ms = *at;
                storage.currencies[count] = out;
                ++count;
            }
            // okx answers one element for the one trading account
            break;
        }
        return std::span<const WireCurrencyBalance>{storage.currencies.data(), count};
    });
}

} // namespace raikiri::venues::okx
