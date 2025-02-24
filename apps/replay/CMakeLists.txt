add_executable(replay main.cpp)
target_link_libraries(replay PRIVATE raikiri::journal raikiri::schema raikiri::domain fmt::fmt
                                     raikiri::warnings)
