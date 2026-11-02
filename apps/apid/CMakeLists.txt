add_executable(apid main.cpp)
target_link_libraries(apid
  PRIVATE raikiri::clientapi raikiri::audit_file raikiri::common raikiri::config raikiri::log OpenSSL::Crypto raikiri::warnings)
