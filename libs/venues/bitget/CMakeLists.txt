add_library(raikiri_venue_bitget STATIC
  src/bitget_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_bitget ALIAS raikiri_venue_bitget)

target_include_directories(raikiri_venue_bitget PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_bitget
  PUBLIC raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
