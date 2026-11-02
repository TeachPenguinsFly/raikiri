add_executable(auditcheck main.cpp)
target_link_libraries(auditcheck PRIVATE fmt::fmt raikiri::audit_file raikiri::common raikiri::warnings)
