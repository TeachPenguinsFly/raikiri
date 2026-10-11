add_executable(symbolaudit main.cpp)
target_link_libraries(symbolaudit
  PRIVATE fmt::fmt raikiri::admin raikiri::domain raikiri::exec raikiri::log raikiri::venue_aster raikiri::venue_backpack
          raikiri::venue_binance raikiri::venue_bingx raikiri::venue_bitfinex raikiri::venue_bitget raikiri::venue_bybit
          raikiri::venue_coinbase raikiri::venue_cryptocom raikiri::venue_blofin raikiri::venue_apex raikiri::venue_deribit
          raikiri::venue_gate raikiri::venue_grvt raikiri::venue_htx raikiri::venue_hyperliquid raikiri::venue_edgex
          raikiri::venue_dydx raikiri::venue_kraken raikiri::venue_kucoin raikiri::venue_lighter raikiri::venue_mexc
          raikiri::venue_okx raikiri::venue_phemex raikiri::venue_extended raikiri::venue_pacifica raikiri::venue_lbank
          raikiri::venue_bitunix raikiri::venue_toobit raikiri::venue_derive raikiri::venue_upbit raikiri::venue_bithumb raikiri::venue_bitstamp raikiri::venue_nado raikiri::venue_weex raikiri::venue_xt
          raikiri::warnings)
