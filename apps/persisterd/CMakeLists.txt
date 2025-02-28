add_executable(persisterd main.cpp)
target_link_libraries(persisterd PRIVATE raikiri::persist fmt::fmt raikiri::warnings)
