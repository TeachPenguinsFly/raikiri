# one target per layer, so an outward dependency fails to link

add_library(raikiri_audit STATIC
  src/chain.cpp
  src/query.cpp
)
add_library(raikiri::audit ALIAS raikiri_audit)
target_include_directories(raikiri_audit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_audit PUBLIC raikiri::common PRIVATE OpenSSL::Crypto raikiri::warnings)

add_library(raikiri_audit_file STATIC
  src/day_index.cpp
  src/file_sink.cpp
  src/file_store.cpp
  src/file_writer.cpp
  src/line.cpp
  src/queue.cpp
  src/verify.cpp
)
add_library(raikiri::audit_file ALIAS raikiri_audit_file)
target_link_libraries(raikiri_audit_file
  PUBLIC raikiri::audit
  PRIVATE fmt::fmt glaze::glaze raikiri::log raikiri::warnings)

add_library(raikiri_audit_http STATIC src/audited.cpp src/routes.cpp)
add_library(raikiri::audit_http ALIAS raikiri_audit_http)
target_link_libraries(raikiri_audit_http
  PUBLIC raikiri::audit raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
