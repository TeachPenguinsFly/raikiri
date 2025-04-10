add_library(raikiri_venue_hyperliquid STATIC
  src/hyperliquid_venue.cpp
)
add_library(raikiri::venue_hyperliquid ALIAS raikiri_venue_hyperliquid)

target_include_directories(raikiri_venue_hyperliquid PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_hyperliquid
  PUBLIC raikiri::domain raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
