add_library(raikiri_exec STATIC
  src/caps.cpp
  src/instrument_table.cpp
  src/order_gate.cpp
  src/reconcile.cpp
  src/venue.cpp
)
add_library(raikiri::exec ALIAS raikiri_exec)

target_include_directories(raikiri_exec PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# exec depends only on domain: venue contracts speak vocabulary types, never
# transport types. (shm reuses the same domain PODs as its slot formats.)
target_link_libraries(raikiri_exec PUBLIC raikiri::domain PRIVATE raikiri::warnings)
