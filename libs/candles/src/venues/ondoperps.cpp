// ondo perps: no reader. /v1/perps/candles answers 401 without an api key
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> ondoperps(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
