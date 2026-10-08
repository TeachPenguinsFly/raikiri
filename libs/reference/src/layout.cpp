#include <raikiri/reference/layout.hpp>

#include <algorithm>
#include <cstring>

namespace raikiri::reference {

std::string_view RefCell::asset_key() const noexcept {
    return {asset, ::strnlen(asset, sizeof asset)};
}

RefCell to_cell(const Row& row, std::uint64_t ts_ns) noexcept {
    const auto& ref = row.ref;
    RefCell cell{.mid = ref.mid,
                 .spot_mid = ref.spot_mid,
                 .perp_mid = ref.perp_mid,
                 .basis_e9 = ref.basis_e9,
                 .conf = ref.conf,
                 .dispersion_e9 = ref.dispersion_e9,
                 .usdt_usd = ref.rates.usdt_usd,
                 .usdc_usd = ref.rates.usdc_usd,
                 .ts_ns = ts_ns,
                 .oldest_age_ns = ref.oldest_age_ns,
                 .quotes = ref.quotes,
                 .used_spot = ref.used_spot,
                 .used_perp = ref.used_perp,
                 .cuts = ref.cuts};
    std::memcpy(cell.asset, row.asset.data(), std::min(row.asset.size(), sizeof cell.asset - 1));
    cell.flags = (ref.refused ? k_flag_refused : 0U) | (ref.rates.usdt_fallback ? k_flag_usdt_fallback : 0U) |
                 (ref.rates.usdc_fallback ? k_flag_usdc_fallback : 0U);
    return cell;
}

} // namespace raikiri::reference
