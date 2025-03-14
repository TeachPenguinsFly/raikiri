add_library(raikiri_venue_sim STATIC src/sim_venue.cpp)
add_library(raikiri::venue_sim ALIAS raikiri_venue_sim)

target_include_directories(raikiri_venue_sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_venue_sim
  PUBLIC raikiri::exec
  PRIVATE raikiri::simcore raikiri::warnings)
