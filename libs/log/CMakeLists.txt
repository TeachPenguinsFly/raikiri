add_library(raikiri_log STATIC src/log.cpp)
add_library(raikiri::log ALIAS raikiri_log)

target_include_directories(raikiri_log PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# quill is PUBLIC by necessity: zero-cost deferred formatting is macro-based,
# so the facade header re-exports quill's logging macros. This is the one
# facade where the backend leaks into the header; swapping it means editing
# exactly one header. Documented trade-off, not an accident.
target_link_libraries(raikiri_log
  PUBLIC quill::quill
  PRIVATE raikiri::warnings)
