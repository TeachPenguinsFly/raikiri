#pragma once

// demo instrument table shared by gatewayd-sim and strategyd; only demo wiring may include it

#include <raikiri/domain/instrument.hpp>

#include <vector>

namespace raikiri::demo {

inline std::vector<Instrument> sim_instruments() {
    const auto make_base = [](std::uint32_t symbol,
                              std::string_view local,
                              std::string_view wire,
                              std::string_view tick,
                              std::string_view lot) {
        InstrumentBase base{};
        base.id = InstrumentId::make(VenueId::Sim, MarketType::Perp, SymbolId{symbol});
        base.local_symbol = *SymbolText::from(local);
        base.exchange_symbol = *SymbolText::from(wire);
        base.tick_size = *parse_decimal_e9(tick);
        base.lot_size = *parse_decimal_e9(lot);
        base.min_notional = *parse_decimal_e9("5");
        base.contract_value = *parse_decimal_e9("1");
        base.kind = ContractKind::Linear;
        return base;
    };
    std::vector<Instrument> out;
    out.push_back(PerpInstrument{make_base(0, "BTC/USDT", "BTCUSDT", "0.1", "0.001"), 28'800});
    out.push_back(PerpInstrument{make_base(1, "ETH/USDT", "ETHUSDT", "0.01", "0.01"), 28'800});
    return out;
}

// Demo starting mids, in ticks of the matching instrument above.
inline constexpr std::int64_t k_demo_mid_ticks[] = {500'000, 250'000}; // 50k.0, 2.5k.00

} // namespace raikiri::demo
