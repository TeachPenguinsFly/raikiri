add_executable(shm_pingpong shm_pingpong.cpp)
target_link_libraries(shm_pingpong PRIVATE raikiri::shm raikiri::warnings)

add_executable(ring_throughput ring_throughput.cpp)
target_link_libraries(ring_throughput PRIVATE raikiri::shm raikiri::domain benchmark::benchmark
                                              raikiri::warnings)
