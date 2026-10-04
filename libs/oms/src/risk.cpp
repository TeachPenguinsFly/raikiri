#include <raikiri/oms/risk.hpp>

#include <array>

namespace raikiri::oms {
namespace internal {

[[nodiscard]] std::string quote(std::int64_t e9) {
    std::array<char, 32> buf{};
    return std::string{format_e9(FixedE9::from_raw(e9), buf.data(), buf.size())};
}

} // namespace internal

std::string describe(const RiskRefusal& refusal) {
    std::string out{to_string(refusal.why)};
    if (refusal.limit != 0) {
        const bool lots = refusal.why == RiskReject::QtyTooLarge || refusal.why == RiskReject::LongLimit ||
                          refusal.why == RiskReject::ShortLimit;
        const auto shown = [&](std::int64_t v) { return lots ? std::to_string(v) + " lots" : internal::quote(v); };
        out += " " + shown(refusal.limit) + ", would reach " + shown(refusal.value);
    }
    if (!refusal.order_notional.is_undef()) {
        out += ", order notional " + internal::quote(refusal.order_notional.raw);
    }
    return out;
}

} // namespace raikiri::oms
