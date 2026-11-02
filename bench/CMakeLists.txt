add_executable(shm_pingpong shm_pingpong.cpp)
target_link_libraries(shm_pingpong
  PRIVATE raikiri::shm raikiri::warnings)

add_executable(ring_throughput ring_throughput.cpp)
target_link_libraries(ring_throughput
  PRIVATE benchmark::benchmark raikiri::domain raikiri::shm raikiri::warnings)

add_executable(ladder_prune ladder_prune.cpp)
target_link_libraries(ladder_prune
  PRIVATE benchmark::benchmark raikiri::book raikiri::warnings)

add_executable(option_board option_board.cpp)
target_link_libraries(option_board
  PRIVATE benchmark::benchmark raikiri::optiondata raikiri::optiondata_http raikiri::warnings)

add_executable(order_trail order_trail.cpp)
target_link_libraries(order_trail
  PRIVATE fmt::fmt raikiri::audit_file raikiri::log raikiri::oms_trail raikiri::warnings)

add_executable(trading_path trading_path.cpp)
target_link_libraries(trading_path
  PRIVATE benchmark::benchmark raikiri::audit_file raikiri::bookrisk raikiri::joe raikiri::journal raikiri::log raikiri::oms raikiri::oms_journal raikiri::oms_trail raikiri::observ raikiri::portfolio raikiri::shm raikiri::warnings)

add_executable(client_path client_path.cpp)
target_include_directories(client_path PRIVATE ${PROJECT_SOURCE_DIR}/tests/clientapi)
target_link_libraries(client_path
  PRIVATE benchmark::benchmark raikiri::clientapi raikiri::log raikiri::net raikiri::warnings)
