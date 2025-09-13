add_library(raikiri_venue_bingx STATIC
  src/bingx_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/client_id.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/account.cpp
  src/book.cpp
  src/catalogue.cpp
  src/envelope.cpp
  src/market_data.cpp
  src/orders.cpp
  src/positions.cpp
)
add_library(raikiri::venue_bingx ALIAS raikiri_venue_bingx)

target_include_directories(raikiri_venue_bingx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Boost::headers is here for beast's header-only zlib: BingX compresses every
# websocket frame, libs/net does not decompress, and boost is the only inflate
# implementation the dependency set already carries. PRIVATE because ws.hpp
# hides it behind a pimpl.
target_link_libraries(raikiri_venue_bingx
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE Boost::headers raikiri::config simdjson::simdjson raikiri::warnings)
