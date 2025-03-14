add_executable(gatewayd main.cpp)
target_link_libraries(gatewayd PRIVATE raikiri::exec raikiri::venue_sim raikiri::shm raikiri::journal
                                       raikiri::schema fmt::fmt raikiri::warnings)
