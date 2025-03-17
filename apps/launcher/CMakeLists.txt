add_executable(launcher main.cpp)
target_link_libraries(launcher PRIVATE fmt::fmt raikiri::warnings)
