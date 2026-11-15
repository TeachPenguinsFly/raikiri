// ostium: no reader. the builder api serves pairs, the price stream and the subgraph but no candles, and a fill
// is priced by the oracle, not by a book
#include "klines.hpp"

namespace raikiri::candles::venues {

std::unique_ptr<KlineVenue> ostium(MarketType /*market*/) {
    return nullptr;
}

} // namespace raikiri::candles::venues
