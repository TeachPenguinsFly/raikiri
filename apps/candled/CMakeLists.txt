add_executable(candled main.cpp)
target_link_libraries(candled
  PRIVATE raikiri::admin raikiri::candles raikiri::net raikiri::common raikiri::log raikiri::warnings)
