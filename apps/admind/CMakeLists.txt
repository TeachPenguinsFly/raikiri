add_executable(admind main.cpp)
target_link_libraries(admind
  PRIVATE raikiri::admin raikiri::books_file raikiri::books_http raikiri::audit_file raikiri::audit_http raikiri::introspect_files raikiri::introspect_http raikiri::reference_shm
    raikiri::reference_http raikiri::optiondata_shm raikiri::optiondata_http raikiri::optionrisk_infra raikiri::optionrisk_http raikiri::common raikiri::log raikiri::warnings)
