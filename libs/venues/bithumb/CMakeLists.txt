add_library(raikiri_venue_bithumb STATIC
  src/venue.cpp
)
add_library(raikiri::venue_bithumb ALIAS raikiri_venue_bithumb)

target_include_directories(raikiri_venue_bithumb PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_bithumb
  PUBLIC raikiri::exec raikiri::venue_upbit
  PRIVATE raikiri::warnings)
