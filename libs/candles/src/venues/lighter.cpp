// lighter: no reader. /api/v1/candles is keyed by market_id, not by the symbol (BTC)
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> lighter(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
