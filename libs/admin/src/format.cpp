#include <raikiri/admin/format.hpp>

#include <array>
#include <charconv>

namespace raikiri::admin {

std::string digits(std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [stop, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return ec == std::errc{} ? std::string{buf.data(), static_cast<std::size_t>(stop - buf.data())}
                             : std::string{};
}

std::string decimal(FixedE9 value) {
    std::array<char, 32> buf{};
    const auto text = format_e9(value, buf.data(), buf.size());
    return std::string{text}; // empty when undefined; see the header
}

std::string price(PriceTicks ticks, const InstrumentBase& base) {
    return decimal(base.ticks_to_price(ticks));
}

std::string size(QtyLots lots, const InstrumentBase& base) {
    return decimal(base.lots_to_qty(lots));
}

std::string_view contract_kind_name(ContractKind kind) noexcept {
    switch (kind) {
    case ContractKind::Linear:
        return "linear";
    case ContractKind::Inverse:
        return "inverse";
    case ContractKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace raikiri::admin
