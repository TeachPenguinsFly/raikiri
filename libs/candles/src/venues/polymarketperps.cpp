// polymarket perps: no reader. /v1/info/klines is keyed by the numeric instrument_id, not by the symbol (BTC-USD)
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> polymarketperps(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
