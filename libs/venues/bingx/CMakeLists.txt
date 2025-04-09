add_library(raikiri_venue_bingx STATIC
  src/bingx_venue.cpp
)
add_library(raikiri::venue_bingx ALIAS raikiri_venue_bingx)

target_include_directories(raikiri_venue_bingx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# simdjson stays PRIVATE: wire.hpp hands out std::string_view and std::span, so
# which JSON library walks the frame is a fact confined to src/.
target_link_libraries(raikiri_venue_bingx
  PUBLIC raikiri::domain raikiri::exec raikiri::net
  PRIVATE simdjson::simdjson raikiri::config raikiri::warnings)
