add_executable(strategyd main.cpp)
target_link_libraries(strategyd
  PRIVATE fmt::fmt raikiri::http raikiri::journal raikiri::log raikiri::observ raikiri::oms
          raikiri::portfolio raikiri::schema raikiri::shm raikiri::strategies raikiri::strategy
          raikiri::strategy_control raikiri::warnings)
