// nado: no reader. the archive's candlesticks query is keyed by product_id, not by the symbol (BTC-PERP)
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> nado(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
