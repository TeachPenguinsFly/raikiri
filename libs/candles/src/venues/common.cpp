#include <raikiri/common/fixed_point.hpp>

#include "klines.hpp"

namespace raikiri::candles::venues {

std::int64_t e9(simdjson::dom::element value) noexcept {
    std::string_view text;
    if (value.is_string()) {
        text = value.get_string().value_unsafe();
    } else if (value.is_double() || value.is_int64() || value.is_uint64()) {
        // a number keeps its digits only as written; simdjson gives it back as a double, so it is printed again
        thread_local std::string buffer;
        const double d = value.is_double()  ? value.get_double().value_unsafe()
                         : value.is_int64() ? static_cast<double>(value.get_int64().value_unsafe())
                                            : static_cast<double>(value.get_uint64().value_unsafe());
        buffer = std::format("{:.9f}", d);
        text = buffer;
    } else {
        return 0;
    }
    // an exact decimal first; a volume printed past nine places (kucoin spot) keeps its first nine
    if (const auto parsed = parse_decimal_e9(text); parsed.has_value()) {
        return parsed->raw;
    }
    const auto truncated = parse_amount_e9(text);
    return truncated.has_value() ? truncated->raw : 0;
}

} // namespace raikiri::candles::venues

namespace raikiri::candles {

std::unique_ptr<KlineVenue> kline_venue(VenueId venue, MarketType market) {
    switch (venue) {
    case VenueId::Binance:
        return venues::binance(market);
    case VenueId::Bybit:
        return venues::bybit(market);
    case VenueId::Okx:
        return venues::okx(market);
    case VenueId::Gate:
        return venues::gate(market);
    case VenueId::Kucoin:
        return venues::kucoin(market);
    case VenueId::Bingx:
        return venues::bingx(market);
    case VenueId::Xt:
        return venues::xt(market);
    case VenueId::Mexc:
        return venues::mexc(market);
    case VenueId::Deribit:
        return venues::deribit(market);
    case VenueId::Hyperliquid:
        return venues::hyperliquid(market);
    case VenueId::Coinbase:
        return venues::coinbase(market);
    case VenueId::Kraken:
        return venues::kraken(market);
    case VenueId::Bitget:
        return venues::bitget(market);
    case VenueId::Htx:
        return venues::htx(market);
    case VenueId::Bitfinex:
        return venues::bitfinex(market);
    case VenueId::Weex:
        return venues::weex(market);
    case VenueId::Aster:
        return venues::aster(market);
    case VenueId::Grvt:
        return venues::grvt(market);
    case VenueId::Lighter:
        return venues::lighter(market);
    case VenueId::Backpack:
        return venues::backpack(market);
    case VenueId::CryptoCom:
        return venues::cryptocom(market);
    case VenueId::Blofin:
        return venues::blofin(market);
    case VenueId::Apex:
        return venues::apex(market);
    case VenueId::Edgex:
        return venues::edgex(market);
    case VenueId::Dydx:
        return venues::dydx(market);
    case VenueId::Phemex:
        return venues::phemex(market);
    case VenueId::Extended:
        return venues::extended(market);
    case VenueId::Pacifica:
        return venues::pacifica(market);
    case VenueId::Lbank:
        return venues::lbank(market);
    case VenueId::Bitunix:
        return venues::bitunix(market);
    case VenueId::Toobit:
        return venues::toobit(market);
    case VenueId::Derive:
        return venues::derive(market);
    case VenueId::Arcus:
        return venues::arcus(market);
    case VenueId::Ostium:
        return venues::ostium(market);
    case VenueId::Paradex:
        return venues::paradex(market);
    case VenueId::Vest:
        return venues::vest(market);
    case VenueId::Bullish:
        return venues::bullish(market);
    case VenueId::OndoPerps:
        return venues::ondoperps(market);
    case VenueId::PolymarketPerps:
        return venues::polymarketperps(market);
    case VenueId::Upbit:
        return venues::upbit(market);
    case VenueId::Bithumb:
        return venues::bithumb(market);
    case VenueId::Bitstamp:
        return venues::bitstamp(market);
    case VenueId::Nado:
        return venues::nado(market);
    default:
        return nullptr;
    }
}

} // namespace raikiri::candles
