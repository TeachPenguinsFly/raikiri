add_executable(admind main.cpp)
target_link_libraries(admind
  PRIVATE raikiri::admin raikiri::common raikiri::warnings)
