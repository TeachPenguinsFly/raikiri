add_library(raikiri_persist STATIC
  src/mapper.cpp
)
add_library(raikiri::persist ALIAS raikiri_persist)

target_include_directories(raikiri_persist PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# libpqxx is confined to src/ behind the PgSink pimpl (facade rule).
target_link_libraries(raikiri_persist
  PUBLIC raikiri::journal raikiri::domain
  PRIVATE raikiri::schema libpqxx::pqxx raikiri::warnings)
