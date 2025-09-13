#include <raikiri/venues/okx/account.hpp>
#include <raikiri/venues/okx/envelope.hpp>

namespace raikiri::venues::okx {

// The shared JSON readers. `raikiri::wire` is the deliberate simdjson
// layer; a venue that reimplements opt_str or req_array is one that will
// disagree with its neighbours about what an absent field means.
namespace detail = raikiri::wire;

namespace {

using detail::guarded;
using detail::session;

std::expected<void, WireError> read_bill(detail::ondemand::object& obj, WireBill& out) {
    // instId is OPTIONAL here and on purpose: a transfer or an interest row
    // names no instrument, and refusing the whole book over one would lose
    // every funding charge beside it.
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
    const auto ts_ms = detail::opt_ms(obj, "ts");
    if (!ts_ms.has_value()) {
        return std::unexpected(WireError::WrongType);
    }
    out.ts_ms = *ts_ms;
    return {};
}

} // namespace

std::expected<std::span<const WireBill>, WireError> parse_bills(std::string_view body,
                                                                BillBuffer& storage) {
    return guarded([&] { return read_data<WireBill>(body, storage.bills, read_bill); });
}

} // namespace raikiri::venues::okx
