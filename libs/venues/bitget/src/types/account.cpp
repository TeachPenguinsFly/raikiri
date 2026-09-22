#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget {

namespace {

using wire::guarded;

std::expected<void, WireError> read_bill(wire::ondemand::object& obj, WireBill& out) {
    out.bill_id = field(obj, "billId");
    out.symbol = field(obj, "symbol");
    out.coin = field(obj, "coin");
    out.business_type = field(obj, "businessType");
    out.amount = field(obj, "amount");
    if (out.bill_id.empty() || out.business_type.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    obj.reset();
    out.c_time_ms = wire::opt_ms(obj, "cTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WireBill>, WireError> parse_mix_bills(std::string_view body,
                                                                    BillBuffer& storage) {
    return guarded([&] { return read_rows<WireBill>(body, "bills", storage.bills, read_bill); });
}

} // namespace raikiri::venues::bitget
