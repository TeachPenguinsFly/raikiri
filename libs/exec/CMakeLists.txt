add_library(raikiri_exec STATIC
  src/caps.cpp
  src/instrument_source.cpp
  src/instrument_table.cpp
  src/order_gate.cpp
  src/rate_budget.cpp
  src/venue.cpp
  src/venue_clock.cpp
)
add_library(raikiri::exec ALIAS raikiri_exec)

target_include_directories(raikiri_exec PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# exec depends only on domain; venue contracts use vocabulary types
target_link_libraries(raikiri_exec
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)
