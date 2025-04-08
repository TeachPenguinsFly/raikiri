add_executable(gatewayd main.cpp)
target_link_libraries(gatewayd PRIVATE raikiri::exec raikiri::venue_sim raikiri::venue_bybit raikiri::venue_binance raikiri::venue_okx raikiri::config raikiri::shm raikiri::journal
                                       raikiri::schema fmt::fmt raikiri::warnings)
