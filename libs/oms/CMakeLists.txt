add_library(raikiri_oms STATIC
  src/attribution.cpp
  src/fee_ledger.cpp
  src/funding_identity.cpp
  src/funding_split.cpp
  src/order_store.cpp
  src/recon.cpp
)
add_library(raikiri::oms ALIAS raikiri_oms)

target_include_directories(raikiri_oms PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_oms
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)
