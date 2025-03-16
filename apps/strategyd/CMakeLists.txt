add_executable(strategyd main.cpp)
target_link_libraries(strategyd PRIVATE raikiri::strategy raikiri::strategies raikiri::oms raikiri::shm
                                        raikiri::journal raikiri::schema raikiri::observ fmt::fmt
                                        raikiri::warnings)
