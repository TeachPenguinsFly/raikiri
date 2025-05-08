add_library(raikiri_venue_kraken STATIC
  src/kraken_venue.cpp
  src/book.cpp
  src/errors.cpp
  src/map.cpp
  src/rest.cpp
  src/ws.cpp
  src/wire_detail.cpp
  src/wire_public.cpp
  src/wire_rest.cpp
)
add_library(raikiri::venue_kraken ALIAS raikiri_venue_kraken)

target_include_directories(raikiri_venue_kraken PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_kraken
  PUBLIC raikiri::domain raikiri::wire raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
