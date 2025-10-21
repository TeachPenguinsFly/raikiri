#include <raikiri/venues/bitget/account.hpp>
#include <raikiri/venues/bitget/envelope.hpp>
#include <raikiri/wire/json.hpp>

namespace raikiri::venues::bitget {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson layer;
// a venue that reimplements opt_str or req_array is one that will disagree
// with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;

std::expected<void, WireError> read_bill(detail::ondemand::object& obj, WireBill& out) {
    out.bill_id = field(obj, "billId");
    out.symbol = field(obj, "symbol");
    out.coin = field(obj, "coin");
    out.business_type = field(obj, "businessType");
    out.amount = field(obj, "amount");
    if (out.bill_id.empty() || out.business_type.empty()) {
        return std::unexpected(WireError::MissingField);
    }
    obj.reset();
    out.c_time_ms = detail::opt_ms(obj, "cTime").value_or(0);
    return {};
}

} // namespace

std::expected<std::span<const WireBill>, WireError> parse_mix_bills(std::string_view body,
                                                                    BillBuffer& storage) {
    return guarded([&] { return read_rows<WireBill>(body, "bills", storage.bills, read_bill); });
}

} // namespace raikiri::venues::bitget
