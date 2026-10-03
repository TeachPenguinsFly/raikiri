add_library(raikiri_net STATIC
  src/inflater.cpp
  src/io_context.cpp
  src/json_writer.cpp
  src/rest_await.cpp
  src/rest_client.cpp
  src/signer.cpp
  src/trust_store.cpp
  src/ws_backoff.cpp
  src/ws_client.cpp
  src/ws_limits.cpp
  src/ws_pool.cpp
  src/ws_rpc.cpp
)
add_library(raikiri::net ALIAS raikiri_net)

target_include_directories(raikiri_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# openssl and boost stay private; no third-party type in a public header
target_link_libraries(raikiri_net
  PUBLIC raikiri::common raikiri::config
  PRIVATE Boost::headers OpenSSL::Crypto OpenSSL::SSL raikiri::log raikiri::warnings)
