// paradex: no reader. /v1/markets/klines counts volume in dollars cut to a whole one, leaves out minutes without
// prints and opens each bar on the last one's close, so it cannot stand in for prints counted in the base asset
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> paradex(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
