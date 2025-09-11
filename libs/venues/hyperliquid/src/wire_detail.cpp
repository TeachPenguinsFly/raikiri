#include "wire_detail.hpp"

namespace raikiri::venues::hyperliquid::detail {

std::expected<void, WireError> read_ctx_fields(ondemand::object& ctx, WireAssetCtx& out) {
    for (const auto& [key, field] : {std::pair{"funding", &out.funding},
                                     std::pair{"markPx", &out.mark_price},
                                     std::pair{"midPx", &out.mid_price},
                                     std::pair{"oraclePx", &out.oracle_price},
                                     std::pair{"prevDayPx", &out.prev_day_price},
                                     std::pair{"openInterest", &out.open_interest},
                                     std::pair{"dayBaseVlm", &out.day_base_volume},
                                     std::pair{"dayNtlVlm", &out.day_notional_volume}}) {
        const auto value = opt_str(ctx, key);
        if (!value.has_value()) {
            return std::unexpected(value.error());
        }
        *field = *value;
    }
    return {};
}

std::expected<std::uint64_t, WireError> opt_ms(ondemand::object& obj, std::string_view key) {
    const auto text = opt_raw(obj, key);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    return digits_to_u64(*text);
}

} // namespace raikiri::venues::hyperliquid::detail
