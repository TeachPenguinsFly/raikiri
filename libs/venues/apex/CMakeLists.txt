add_library(raikiri_venue_apex STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/book.cpp
  src/types/map.cpp
  src/types/trade.cpp
  src/types/wire.cpp
  src/types/ws.cpp
  src/zk.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_apex ALIAS raikiri_venue_apex)

target_include_directories(raikiri_venue_apex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_apex
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE OpenSSL::Crypto raikiri::log simdjson::simdjson raikiri::warnings)
