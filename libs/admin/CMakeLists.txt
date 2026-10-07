add_library(raikiri_admin STATIC
  src/desk.cpp
  src/format.cpp
  src/history.cpp
  src/registry.cpp
  src/routes.cpp
  src/stream.cpp
  src/stream_token.cpp
)
add_library(raikiri::admin ALIAS raikiri_admin)

target_include_directories(raikiri_admin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_admin
  PUBLIC raikiri::domain raikiri::shm raikiri::http raikiri::config
  PRIVATE Boost::headers raikiri::net OpenSSL::Crypto glaze::glaze libpqxx::pqxx raikiri::warnings)
