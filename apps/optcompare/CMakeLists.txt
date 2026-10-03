add_executable(optcompare main.cpp)
target_link_libraries(optcompare
  PRIVATE fmt::fmt raikiri::domain raikiri::options raikiri::shm raikiri::warnings)
