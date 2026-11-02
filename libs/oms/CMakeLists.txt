add_library(raikiri_oms STATIC
  src/attribution.cpp
  src/cross_record.cpp
  src/exec_path.cpp
  src/fee_ledger.cpp
  src/funding_identity.cpp
  src/funding_split.cpp
  src/notional.cpp
  src/order_commands.cpp
  src/order_store.cpp
  src/recon.cpp
  src/repeat_guard.cpp
  src/risk.cpp
  src/short_option.cpp
)
add_library(raikiri::oms ALIAS raikiri_oms)

target_include_directories(raikiri_oms PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(raikiri_oms
  PUBLIC raikiri::domain
  PRIVATE raikiri::warnings)

add_library(raikiri_oms_journal STATIC src/replay.cpp)
add_library(raikiri::oms_journal ALIAS raikiri_oms_journal)
target_link_libraries(raikiri_oms_journal
  PUBLIC raikiri::oms raikiri::journal
  PRIVATE raikiri::schema raikiri::warnings)

add_library(raikiri_oms_trail STATIC src/order_trail.cpp)
add_library(raikiri::oms_trail ALIAS raikiri_oms_trail)
target_link_libraries(raikiri_oms_trail
  PUBLIC raikiri::oms raikiri::audit
  PRIVATE fmt::fmt raikiri::warnings)
