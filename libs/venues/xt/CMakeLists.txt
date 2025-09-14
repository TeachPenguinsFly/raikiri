add_library(raikiri_venue_xt STATIC
  src/xt_venue.cpp
  src/account.cpp
  src/book.cpp
  src/catalogue.cpp
  src/client_id.cpp
  src/data_client.cpp
  src/envelope.cpp
  src/errors.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/map.cpp
  src/market_data.cpp
  src/orders.cpp
  src/positions.cpp
  src/rest.cpp
  src/ws.cpp
)
add_library(raikiri::venue_xt ALIAS raikiri_venue_xt)

target_include_directories(raikiri_venue_xt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_xt
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
