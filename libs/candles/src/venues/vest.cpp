// vest: no reader. /v2/klines answers zero volume on every bar and ignores startTime and endTime, so it holds no
// prints to fill a gap with
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> vest(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
