include(GoogleTest)

function(raikiri_add_test name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main raikiri::warnings)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

raikiri_add_test(histogram_test SRCS observ/histogram_test.cpp LIBS raikiri::observ)

raikiri_add_test(fixed_point_test SRCS common/fixed_point_test.cpp LIBS raikiri::common)
raikiri_add_test(fixed_string_test SRCS common/fixed_string_test.cpp LIBS raikiri::common)
raikiri_add_test(ulid_test SRCS common/ulid_test.cpp LIBS raikiri::common)
raikiri_add_test(platform_test SRCS common/platform_test.cpp LIBS raikiri::common)

raikiri_add_test(identifiers_test SRCS domain/identifiers_test.cpp LIBS raikiri::domain)
raikiri_add_test(instrument_test SRCS domain/instrument_test.cpp LIBS raikiri::domain)
raikiri_add_test(order_fsm_test SRCS domain/order_fsm_test.cpp LIBS raikiri::domain)

raikiri_add_test(journal_test SRCS journal/journal_test.cpp LIBS raikiri::journal raikiri::schema)

raikiri_add_test(mapper_test SRCS persist/mapper_test.cpp LIBS raikiri::persist raikiri::schema)
target_compile_definitions(mapper_test PRIVATE RAIKIRI_SQL_DIR="${PROJECT_SOURCE_DIR}/sql")

raikiri_add_test(secret_test SRCS config/secret_test.cpp LIBS raikiri::config)

raikiri_add_test(json_writer_test SRCS net/json_writer_test.cpp LIBS raikiri::net)
raikiri_add_test(signer_test SRCS net/signer_test.cpp LIBS raikiri::net)

raikiri_add_test(bybit_book_sync_test SRCS venues/bybit/book_sync_test.cpp LIBS raikiri::venue_bybit)
raikiri_add_test(bybit_map_test SRCS venues/bybit/map_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(bybit_rest_test SRCS venues/bybit/rest_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_ws_test SRCS venues/bybit/ws_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_venue_test SRCS venues/bybit/venue_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(bybit_wire_test SRCS venues/bybit/wire_test.cpp LIBS raikiri::venue_bybit)
target_compile_definitions(bybit_wire_test PRIVATE
  RAIKIRI_BYBIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/bybit/fixtures")

raikiri_add_test(ws_client_test SRCS net/ws_client_test.cpp LIBS raikiri::net)
raikiri_add_test(rest_client_test SRCS net/rest_client_test.cpp LIBS raikiri::net)
target_link_libraries(ws_client_test PRIVATE Boost::headers OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(rest_client_test PRIVATE Boost::headers OpenSSL::SSL OpenSSL::Crypto)

raikiri_add_test(venue_contract_test SRCS exec/venue_contract_test.cpp LIBS raikiri::exec)

raikiri_add_test(order_store_test SRCS oms/order_store_test.cpp LIBS raikiri::oms)
raikiri_add_test(oms_risk_test SRCS oms/risk_test.cpp LIBS raikiri::oms)

raikiri_add_test(spsc_ring_test SRCS shm/spsc_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(seqlock_test SRCS shm/seqlock_test.cpp LIBS raikiri::shm)
raikiri_add_test(broadcast_ring_test SRCS shm/broadcast_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(segment_test SRCS shm/segment_test.cpp LIBS raikiri::shm)
raikiri_add_test(catalogue_test SRCS shm/catalogue_test.cpp LIBS raikiri::shm)

raikiri_add_test(sim_book_test SRCS simcore/book_test.cpp LIBS raikiri::simcore)
raikiri_add_test(sim_engine_test SRCS simcore/engine_test.cpp LIBS raikiri::simcore)
