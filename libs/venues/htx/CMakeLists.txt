add_library(raikiri_venue_htx STATIC
  src/htx_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/sign.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_htx ALIAS raikiri_venue_htx)

target_include_directories(raikiri_venue_htx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Boost::headers is here for beast's header-only zlib: HTX compresses every
# websocket frame, libs/net does not decompress, and boost is the only inflate
# implementation the dependency set already carries. PRIVATE because ws.hpp
# hides it behind a pimpl.
target_link_libraries(raikiri_venue_htx
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE Boost::headers raikiri::config simdjson::simdjson raikiri::warnings)
