add_executable(strategyd main.cpp)
target_link_libraries(strategyd
  PRIVATE fmt::fmt glaze::glaze raikiri::audit_file raikiri::audit_http raikiri::bookrisk raikiri::bookrisk_http raikiri::bookrisk_infra raikiri::books_file raikiri::config raikiri::http raikiri::internalcross raikiri::internalcross_http raikiri::internalcross_infra raikiri::portfolio_journal raikiri::joe raikiri::joe_http raikiri::joe_infra raikiri::journal raikiri::log raikiri::observ raikiri::oms raikiri::oms_journal raikiri::oms_trail raikiri::optionrisk raikiri::optionrisk_http
          raikiri::portfolio raikiri::schema raikiri::shm raikiri::strategies raikiri::strategy
          raikiri::strategy_control raikiri::warnings)
