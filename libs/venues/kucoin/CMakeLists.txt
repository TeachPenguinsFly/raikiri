add_library(raikiri_venue_kucoin STATIC
  src/kucoin_venue.cpp
  src/data_client.cpp
  src/exec_client.cpp
  src/instruments.cpp
  src/book.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_private.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_kucoin ALIAS raikiri_venue_kucoin)

target_include_directories(raikiri_venue_kucoin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_kucoin
  PUBLIC raikiri::book raikiri::domain raikiri::exec raikiri::net raikiri::wire
  PRIVATE raikiri::config simdjson::simdjson raikiri::warnings)
