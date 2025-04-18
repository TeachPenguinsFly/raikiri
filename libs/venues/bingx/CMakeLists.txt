add_library(raikiri_venue_bingx STATIC
  src/bingx_venue.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_bingx ALIAS raikiri_venue_bingx)

target_include_directories(raikiri_venue_bingx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
#
# Boost::headers is here for beast's header-only zlib: BingX compresses every
# websocket frame, libs/net does not decompress, and boost is the only inflate
# implementation the dependency set already carries. It stays PRIVATE for the
# same reason simdjson does — ws.hpp hides it behind a pimpl.
target_link_libraries(raikiri_venue_bingx
  PUBLIC raikiri::domain raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson Boost::headers raikiri::config raikiri::warnings)
