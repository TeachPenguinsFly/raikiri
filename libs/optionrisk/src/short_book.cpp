#include <raikiri/optionrisk/short_book.hpp>
#include <raikiri/options/key.hpp>

namespace raikiri::optionrisk {

std::optional<Holding> holding_of(const OptionInstrument& option, QtyLots net, UnixNanos now) noexcept {
    const auto key = options::key_of(option);
    if (!key.has_value()) {
        return std::nullopt;
    }
    Holding out{};
    out.instrument = option.base.id;
    out.symbol = option.base.exchange_symbol;
    out.key = *key;
    out.premium = option.premium;
    out.contracts = option.base.lots_to_qty(net);
    out.contract_value = option.base.contract_value;
    out.as_of = now;
    return out;
}

MarketState market_of(const OptionGreeks& greeks) noexcept {
    return {.mark = greeks.mark_price,
            .mark_from = MarkFrom::Venue,
            .mark_ts = greeks.ts_event,
            .iv = greeks.mark_iv,
            .greeks_ts = greeks.ts_event,
            .forward = greeks.underlying_price,
            .rate = greeks.index_price};
}

BookGreeks book_greeks(std::span<const PositionRisk> positions) noexcept {
    BookGreeks out{};
    for (const auto& position : positions) {
        if (position.vega_usd.is_undef() || position.gamma_usd_1pct.is_undef()) {
            ++out.incomplete;
            continue;
        }
        out.vega_usd = FixedE9::from_raw(out.vega_usd.raw + position.vega_usd.raw);
        out.gamma_usd_1pct = FixedE9::from_raw(out.gamma_usd_1pct.raw + position.gamma_usd_1pct.raw);
    }
    return out;
}

bool past_short_limits(const BookGreeks& greeks, FixedE9 max_vega, FixedE9 max_gamma) noexcept {
    if (max_vega.is_undef() && max_gamma.is_undef()) {
        return false;
    }
    if (greeks.incomplete != 0) {
        return true;
    }
    return (!max_vega.is_undef() && -greeks.vega_usd.raw > max_vega.raw) ||
           (!max_gamma.is_undef() && -greeks.gamma_usd_1pct.raw > max_gamma.raw);
}

} // namespace raikiri::optionrisk
