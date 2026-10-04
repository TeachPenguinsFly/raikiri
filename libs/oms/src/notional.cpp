#include <raikiri/oms/notional.hpp>

#include <limits>
#include <variant>

namespace raikiri::oms {
namespace internal {

constexpr int128_t k_saturated = static_cast<int128_t>(~uint128_t{0} >> 1);

[[nodiscard]] int128_t mul_saturated(int128_t a, int128_t b) noexcept {
    int128_t out{};
    return __builtin_mul_overflow(a, b, &out) ? k_saturated : out;
}

} // namespace internal

std::expected<Notional128, NotionalError>
notional(const Instrument& instrument, PriceTicks price, QtyLots lots) noexcept {
    const auto& base = base_of(instrument);
    if (lots.is_undef() || base.lot_size.raw <= 0 || base.contract_value.is_undef() || base.contract_value.raw <= 0) {
        return std::unexpected{NotionalError::Unpriceable};
    }
    const auto* option = std::get_if<OptionInstrument>(&instrument);
    if (option != nullptr ? option->premium != OptionUnit::Quote || base.kind != ContractKind::Linear
                          : base.kind == ContractKind::Unknown) {
        return std::unexpected{NotionalError::Unpriceable};
    }
    const int128_t magnitude = lots.raw < 0 ? -static_cast<int128_t>(lots.raw) : lots.raw;
    const auto units = internal::mul_saturated(magnitude, base.lot_size.raw);
    // base units for linear, quote units for inverse
    const auto face = internal::mul_saturated(units, base.contract_value.raw) / k_e9;
    if (base.kind == ContractKind::Inverse) {
        return Notional128{face};
    }
    if (price.is_undef() || price.raw <= 0) {
        return std::unexpected{NotionalError::NoPrice};
    }
    const auto px = internal::mul_saturated(price.raw, base.tick_size.raw);
    if (px <= 0) {
        return std::unexpected{NotionalError::Unpriceable};
    }
    return Notional128{internal::mul_saturated(face, px) / k_e9};
}

FixedE9 to_e9(Notional128 value) noexcept {
    constexpr auto k_max = std::numeric_limits<std::int64_t>::max();
    return FixedE9::from_raw(value.raw > k_max ? k_max : static_cast<std::int64_t>(value.raw));
}

} // namespace raikiri::oms
