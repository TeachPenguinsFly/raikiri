add_executable(gatewayd main.cpp)
target_link_libraries(gatewayd
  PRIVATE fmt::fmt raikiri::config raikiri::exec raikiri::journal raikiri::log raikiri::schema
          raikiri::shm raikiri::venue_aster raikiri::venue_backpack raikiri::venue_binance raikiri::venue_bingx raikiri::venue_bitfinex
          raikiri::venue_bitget raikiri::venue_bybit raikiri::venue_coinbase raikiri::venue_cryptocom raikiri::venue_blofin
          raikiri::venue_deribit raikiri::venue_gate raikiri::venue_grvt raikiri::venue_htx raikiri::venue_hyperliquid
          raikiri::venue_kraken raikiri::venue_kucoin raikiri::venue_lighter raikiri::venue_mexc raikiri::venue_okx
          raikiri::venue_sim raikiri::venue_weex raikiri::venue_xt raikiri::warnings)
