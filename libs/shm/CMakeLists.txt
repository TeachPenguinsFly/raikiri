add_library(raikiri_shm STATIC
  src/catalogue.cpp
  src/records.cpp
  src/segment.cpp
)
add_library(raikiri::shm ALIAS raikiri_shm)

target_include_directories(raikiri_shm PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_shm PUBLIC raikiri::common raikiri::domain PRIVATE raikiri::warnings)
