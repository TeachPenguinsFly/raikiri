# one target per layer, so an outward dependency fails to link

add_library(raikiri_books STATIC src/book.cpp src/registry.cpp)
add_library(raikiri::books ALIAS raikiri_books)
target_include_directories(raikiri_books PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_books PUBLIC raikiri::domain PRIVATE raikiri::warnings)

add_library(raikiri_books_file STATIC src/file_store.cpp)
add_library(raikiri::books_file ALIAS raikiri_books_file)
target_link_libraries(raikiri_books_file
  PUBLIC raikiri::books
  PRIVATE glaze::glaze raikiri::warnings)

add_library(raikiri_books_http STATIC src/routes.cpp)
add_library(raikiri::books_http ALIAS raikiri_books_http)
target_link_libraries(raikiri_books_http
  PUBLIC raikiri::books raikiri::http
  PRIVATE glaze::glaze raikiri::warnings)
