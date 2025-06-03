add_library(raikiri_venue_mexc STATIC
  src/errors.cpp
  src/map.cpp
  src/mexc_venue.cpp
  src/rest.cpp
  src/wire_detail.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
  src/ws.cpp
)
add_library(raikiri::venue_mexc ALIAS raikiri_venue_mexc)

target_include_directories(raikiri_venue_mexc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_mexc
  PUBLIC raikiri::domain raikiri::wire raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
