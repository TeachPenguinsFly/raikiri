add_library(raikiri_candles STATIC
  src/builder.cpp
  src/deep.cpp
  src/funding.cpp
  src/history.cpp
  src/metrics.cpp
  src/store.cpp
  src/venues/apex.cpp
  src/venues/arcus.cpp
  src/venues/aster.cpp
  src/venues/backpack.cpp
  src/venues/binance.cpp
  src/venues/bingx.cpp
  src/venues/bitfinex.cpp
  src/venues/bitget.cpp
  src/venues/bithumb.cpp
  src/venues/bitstamp.cpp
  src/venues/bitunix.cpp
  src/venues/blofin.cpp
  src/venues/bullish.cpp
  src/venues/bybit.cpp
  src/venues/coinbase.cpp
  src/venues/common.cpp
  src/venues/cryptocom.cpp
  src/venues/deribit.cpp
  src/venues/derive.cpp
  src/venues/dydx.cpp
  src/venues/edgex.cpp
  src/venues/extended.cpp
  src/venues/funding.cpp
  src/venues/gate.cpp
  src/venues/grvt.cpp
  src/venues/htx.cpp
  src/venues/hyperliquid.cpp
  src/venues/kraken.cpp
  src/venues/kucoin.cpp
  src/venues/lbank.cpp
  src/venues/lighter.cpp
  src/venues/mexc.cpp
  src/venues/nado.cpp
  src/venues/okx.cpp
  src/venues/ondoperps.cpp
  src/venues/ostium.cpp
  src/venues/pacifica.cpp
  src/venues/paradex.cpp
  src/venues/phemex.cpp
  src/venues/polymarketperps.cpp
  src/venues/toobit.cpp
  src/venues/upbit.cpp
  src/venues/vest.cpp
  src/venues/weex.cpp
  src/venues/xt.cpp
)
add_library(raikiri::candles ALIAS raikiri_candles)

target_include_directories(raikiri_candles PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# libpqxx stays in src/ behind the Store pimpl
target_link_libraries(raikiri_candles
  PUBLIC raikiri::common raikiri::domain
  PRIVATE libpqxx::pqxx simdjson::simdjson raikiri::warnings)
