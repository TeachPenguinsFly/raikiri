add_library(raikiri_venue_blofin STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_blofin ALIAS raikiri_venue_blofin)

target_include_directories(raikiri_venue_blofin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_blofin
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
