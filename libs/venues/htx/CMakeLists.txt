add_library(raikiri_venue_htx STATIC
  src/venue.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/types/account.cpp
  src/types/book.cpp
  src/types/catalogue.cpp
  src/types/envelope.cpp
  src/types/errors.cpp
  src/types/map.cpp
  src/types/market_data.cpp
  src/types/orders.cpp
  src/types/positions.cpp
  src/types/sign.cpp
  src/types/ws.cpp
  src/marketdata/client.cpp
  src/rest/builder.cpp
)
add_library(raikiri::venue_htx ALIAS raikiri_venue_htx)

target_include_directories(raikiri_venue_htx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# boost supplies the inflate for htx's compressed frames; private because ws.hpp hides it
target_link_libraries(raikiri_venue_htx
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE Boost::headers raikiri::config simdjson::simdjson raikiri::warnings)
