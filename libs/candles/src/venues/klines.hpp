#pragma once
// what each venue's history file needs: the factory sees only the constructors

#include <raikiri/candles/history.hpp>

#include <simdjson.h>

#include <format>

namespace raikiri::candles::venues {

// a decimal the venue sent as a string or a number, as e9; zero when unreadable
[[nodiscard]] std::int64_t e9(simdjson::dom::element value) noexcept;

// one per venue, each in its own file: null for a market the venue keeps no public history of
[[nodiscard]] std::unique_ptr<KlineVenue> binance(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bybit(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> okx(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> gate(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> kucoin(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bingx(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> xt(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> mexc(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> deribit(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> hyperliquid(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> coinbase(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> kraken(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bitget(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> htx(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bitfinex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> weex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> aster(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> grvt(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> lighter(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> backpack(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> cryptocom(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> blofin(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> apex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> edgex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> dydx(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> phemex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> extended(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> pacifica(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> lbank(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bitunix(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> toobit(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> derive(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> arcus(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> ostium(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> paradex(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> vest(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bullish(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> ondoperps(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> polymarketperps(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> upbit(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bithumb(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> bitstamp(MarketType market);
[[nodiscard]] std::unique_ptr<KlineVenue> nado(MarketType market);

} // namespace raikiri::candles::venues
