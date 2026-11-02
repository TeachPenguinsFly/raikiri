add_library(raikiri_clientapi STATIC
  src/admin_backend.cpp
  src/auth.cpp
  src/book.cpp
  src/desk.cpp
  src/edge_server.cpp
  src/feed.cpp
  src/file_ledger.cpp
  src/frames.cpp
  src/guards.cpp
  src/hub.cpp
  src/key.cpp
  src/key_file.cpp
  src/router.cpp
)
add_library(raikiri::clientapi ALIAS raikiri_clientapi)

target_include_directories(raikiri_clientapi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_clientapi
  PUBLIC raikiri::audit raikiri::common raikiri::domain raikiri::config raikiri::portfolio
  PRIVATE raikiri::books raikiri::net raikiri::log Boost::headers OpenSSL::Crypto glaze::glaze raikiri::warnings)
