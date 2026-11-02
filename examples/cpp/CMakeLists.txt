cmake_minimum_required(VERSION 3.28)
project(raikiri_examples LANGUAGES CXX)

if(NOT TARGET raikiri::sdk)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  find_package(raikiri-sdk REQUIRED CONFIG)
endif()

foreach(example place_and_cancel stream_quotes stream_fills twap sor_slippage option_chain market_maker stale_price_guard)
  add_executable(example_${example} ${example}.cpp)
  target_link_libraries(example_${example} PRIVATE raikiri::sdk)
endforeach()
