#include <raikiri/venues/weex/account.hpp>
#include <raikiri/venues/weex/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::weex {

namespace {

using wire::guarded;
namespace ondemand = wire::ondemand;

constexpr std::string_view k_income_funding = "position_funding";

std::expected<void, WireError> read_balance(ondemand::object& obj, WireBalance& out) {
    auto asset = wire::req_str(obj, "asset");
    if (!asset.has_value()) {
        return std::unexpected{asset.error()};
    }
    out.asset = *asset;
    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    out.balance = field("balance");
    out.available_balance = field("availableBalance");
    out.unrealize_pnl = field("unrealizePnl");
    return {};
}

std::expected<void, WireError> read_bill(ondemand::object& obj, WireBill& out) {
    auto income_type = wire::req_str(obj, "incomeType");
    if (!income_type.has_value()) {
        return std::unexpected{income_type.error()};
    }
    out.income_type = *income_type;
    const auto field = [&](std::string_view key) {
        obj.reset();
        return wire::opt_str(obj, key).value_or(std::string_view{});
    };
    out.symbol = field("symbol");
    out.income = field("income");
    obj.reset();
    // an 18-digit json number
    auto raw = wire::opt_raw(obj, "billId").value_or(std::string_view{});
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    out.bill_id = raw;
    obj.reset();
    out.time_ms = wire::opt_u64(obj, "time").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WireBalance>, WireError> parse_balances(std::string_view body,
                                                                      BalanceBuffer& storage) {
    return guarded([&] { return read_array<WireBalance>(body, storage.balances, read_balance); });
}

std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                BillBuffer& storage) {
    return guarded([&] { return read_list<WireBill>(body, "items", storage.bills, read_bill); });
}

bool is_funding_bill(const WireBill& wire) noexcept {
    return wire.income_type == k_income_funding;
}

} // namespace raikiri::venues::weex
