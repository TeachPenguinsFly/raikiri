add_library(raikiri_admin STATIC
  src/format.cpp
  src/history.cpp
  src/registry.cpp
  src/routes.cpp
  src/server.cpp
  src/stream.cpp
)
add_library(raikiri::admin ALIAS raikiri_admin)

target_include_directories(raikiri_admin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_admin
  PUBLIC raikiri::domain raikiri::shm
  PRIVATE Boost::headers glaze::glaze libpqxx::pqxx raikiri::warnings)
