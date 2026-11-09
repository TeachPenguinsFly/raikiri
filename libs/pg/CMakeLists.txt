add_library(raikiri_pg STATIC
  src/pool.cpp
)
add_library(raikiri::pg ALIAS raikiri_pg)

# conan's libpq declares no include directory, though the package holds the headers; its toolchain lists the package's
# include folder in CMAKE_INCLUDE_PATH, which find_path searches first
find_path(RAIKIRI_LIBPQ_INCLUDE libpq-fe.h REQUIRED)

target_include_directories(raikiri_pg PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(raikiri_pg SYSTEM PRIVATE ${RAIKIRI_LIBPQ_INCLUDE})
# libpq and asio stay in src/; the header names neither
target_link_libraries(raikiri_pg
  PUBLIC raikiri::net
  PRIVATE PostgreSQL::PostgreSQL Boost::headers raikiri::warnings)
