add_executable(admind main.cpp)
target_link_libraries(admind
  PRIVATE raikiri::admin raikiri::introspect_files raikiri::introspect_http raikiri::common raikiri::log raikiri::warnings)
