add_library(raikiri_net STATIC
  src/inflater.cpp
  src/io_context.cpp
  src/json_writer.cpp
  src/rest_client.cpp
  src/signer.cpp
  src/trust_store.cpp
  src/ws_client.cpp
)
add_library(raikiri::net ALIAS raikiri_net)

target_include_directories(raikiri_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# OpenSSL and Boost stay PRIVATE: no third-party type appears in a public
# header of this facade (the io_context wrapper is a pimpl for that reason).
target_link_libraries(raikiri_net
  PUBLIC raikiri::common raikiri::config
  PRIVATE OpenSSL::SSL OpenSSL::Crypto Boost::headers raikiri::warnings)
