add_library(raikiri_venue_extended STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/marketdata/client.cpp
)
add_library(raikiri::venue_extended ALIAS raikiri_venue_extended)

target_include_directories(raikiri_venue_extended PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_extended
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::log simdjson::simdjson raikiri::warnings)
