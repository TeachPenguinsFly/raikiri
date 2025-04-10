add_executable(gatewayd main.cpp)
target_link_libraries(gatewayd PRIVATE raikiri::exec raikiri::venue_sim raikiri::venue_bybit raikiri::venue_binance raikiri::venue_deribit raikiri::venue_hyperliquid raikiri::venue_coinbase raikiri::venue_kraken raikiri::venue_bitmex raikiri::venue_gate raikiri::venue_kucoin raikiri::venue_bingx raikiri::venue_xt raikiri::venue_mexc raikiri::venue_okx raikiri::config raikiri::shm raikiri::journal
                                       raikiri::schema fmt::fmt raikiri::warnings)
