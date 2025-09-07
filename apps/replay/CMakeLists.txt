add_executable(replay main.cpp)
target_link_libraries(replay
  PRIVATE fmt::fmt raikiri::domain raikiri::journal raikiri::schema raikiri::warnings)
