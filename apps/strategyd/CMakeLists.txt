add_executable(strategyd main.cpp)
target_link_libraries(strategyd
  PRIVATE fmt::fmt glaze::glaze raikiri::audit_file raikiri::audit_http raikiri::config raikiri::http raikiri::joe raikiri::joe_http raikiri::joe_infra raikiri::journal raikiri::log raikiri::observ raikiri::oms
          raikiri::portfolio raikiri::schema raikiri::shm raikiri::strategies raikiri::strategy
          raikiri::strategy_control raikiri::warnings)
