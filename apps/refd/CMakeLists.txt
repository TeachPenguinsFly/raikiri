add_executable(refd main.cpp)
target_link_libraries(refd
  PRIVATE raikiri::reference_shm raikiri::common raikiri::log raikiri::warnings)
