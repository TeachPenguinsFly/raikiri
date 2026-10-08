add_executable(reftop main.cpp)
target_link_libraries(reftop
  PRIVATE raikiri::reference_shm raikiri::reference_http fmt::fmt raikiri::warnings)
