add_executable(kowalski main.cpp)
target_link_libraries(kowalski
  PRIVATE raikiri::domain raikiri::http raikiri::log raikiri::scan raikiri::shm
          glaze::glaze raikiri::warnings)
