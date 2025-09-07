add_executable(persisterd main.cpp)
target_link_libraries(persisterd
  PRIVATE fmt::fmt raikiri::persist raikiri::warnings)
