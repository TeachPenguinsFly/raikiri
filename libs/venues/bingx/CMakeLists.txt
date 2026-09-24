add_library(raikiri_venue_bingx STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/account.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/client_id.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/positions.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_bingx ALIAS raikiri_venue_bingx)

target_include_directories(raikiri_venue_bingx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# boost::headers supplies beast's zlib for bingx's compressed frames
target_link_libraries(raikiri_venue_bingx
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE Boost::headers raikiri::config simdjson::simdjson raikiri::warnings)
