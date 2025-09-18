include(GoogleTest)

function(raikiri_add_test name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name}
    PRIVATE ${ARG_LIBS} GTest::gtest_main raikiri::warnings)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

raikiri_add_test(histogram_test SRCS observ/histogram_test.cpp LIBS raikiri::observ)

raikiri_add_test(fixed_point_test SRCS common/fixed_point_test.cpp LIBS raikiri::common)
raikiri_add_test(fixed_string_test SRCS common/fixed_string_test.cpp LIBS raikiri::common)
raikiri_add_test(ulid_test SRCS common/ulid_test.cpp LIBS raikiri::common)
raikiri_add_test(platform_test SRCS common/platform_test.cpp LIBS raikiri::common)
raikiri_add_test(time_test SRCS common/time_test.cpp LIBS raikiri::common)

raikiri_add_test(identifiers_test SRCS domain/identifiers_test.cpp LIBS raikiri::domain)
raikiri_add_test(instrument_test SRCS domain/instrument_test.cpp LIBS raikiri::domain)
raikiri_add_test(fill_identity_test SRCS domain/fill_identity_test.cpp LIBS raikiri::domain)
raikiri_add_test(symbol_test SRCS domain/symbol_test.cpp LIBS raikiri::domain)
raikiri_add_test(order_fsm_test SRCS domain/order_fsm_test.cpp LIBS raikiri::domain)

raikiri_add_test(journal_test SRCS journal/journal_test.cpp LIBS raikiri::journal raikiri::schema raikiri::domain)

raikiri_add_test(mapper_test SRCS persist/mapper_test.cpp LIBS raikiri::persist raikiri::schema)
target_compile_definitions(mapper_test PRIVATE RAIKIRI_SQL_DIR="${PROJECT_SOURCE_DIR}/sql")

raikiri_add_test(credentials_test SRCS config/credentials_test.cpp LIBS raikiri::config)
raikiri_add_test(secret_test SRCS config/secret_test.cpp LIBS raikiri::config)

raikiri_add_test(json_writer_test SRCS net/json_writer_test.cpp LIBS raikiri::net)
raikiri_add_test(protobuf_test SRCS wire/protobuf_test.cpp LIBS raikiri::wire)

raikiri_add_test(admin_test SRCS admin/admin_test.cpp LIBS raikiri::admin)
target_link_libraries(admin_test
  PRIVATE glaze::glaze)

raikiri_add_test(settings_test SRCS config/settings_test.cpp LIBS raikiri::config)
# The committed config files ARE the fixture: this test exists to catch a file
# the shipped loader cannot read, so it must point at the real ones rather than
# at a copy that can drift away from them.
target_compile_definitions(settings_test PRIVATE RAIKIRI_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")

raikiri_add_test(signer_test SRCS net/signer_test.cpp LIBS raikiri::net)

raikiri_add_test(gate_venue_test SRCS venues/gate/venue_test.cpp LIBS raikiri::venue_gate raikiri::exec)
raikiri_add_test(kucoin_venue_test SRCS venues/kucoin/venue_test.cpp LIBS raikiri::venue_kucoin raikiri::exec)
raikiri_add_test(bingx_venue_test SRCS venues/bingx/venue_test.cpp LIBS raikiri::venue_bingx raikiri::exec)
raikiri_add_test(xt_venue_test SRCS venues/xt/venue_test.cpp LIBS raikiri::venue_xt raikiri::exec)
raikiri_add_test(mexc_venue_test SRCS venues/mexc/venue_test.cpp LIBS raikiri::venue_mexc raikiri::exec)
raikiri_add_test(deribit_venue_test SRCS venues/deribit/venue_test.cpp LIBS raikiri::venue_deribit raikiri::exec)
raikiri_add_test(hyperliquid_venue_test SRCS venues/hyperliquid/venue_test.cpp LIBS raikiri::venue_hyperliquid raikiri::exec)
raikiri_add_test(coinbase_venue_test SRCS venues/coinbase/venue_test.cpp LIBS raikiri::venue_coinbase raikiri::exec)
raikiri_add_test(kraken_venue_test SRCS venues/kraken/venue_test.cpp LIBS raikiri::venue_kraken raikiri::exec)
raikiri_add_test(bitmex_venue_test SRCS venues/bitmex/venue_test.cpp LIBS raikiri::venue_bitmex raikiri::exec)
raikiri_add_test(bitget_venue_test SRCS venues/bitget/venue_test.cpp LIBS raikiri::venue_bitget raikiri::exec)
raikiri_add_test(htx_venue_test SRCS venues/htx/venue_test.cpp LIBS raikiri::venue_htx raikiri::exec)
raikiri_add_test(bitfinex_venue_test SRCS venues/bitfinex/venue_test.cpp LIBS raikiri::venue_bitfinex raikiri::exec)
raikiri_add_test(ladder_test SRCS book/ladder_test.cpp LIBS raikiri::book)
raikiri_add_test(order_gate_test SRCS exec/order_gate_test.cpp LIBS raikiri::exec)
raikiri_add_test(rate_budget_test SRCS exec/rate_budget_test.cpp LIBS raikiri::exec)
raikiri_add_test(binance_map_test SRCS venues/binance/map_test.cpp LIBS raikiri::venue_binance raikiri::exec)
raikiri_add_test(binance_rest_test SRCS venues/binance/rest_test.cpp LIBS raikiri::venue_binance raikiri::net)

raikiri_add_test(binance_wire_test SRCS venues/binance/wire_test.cpp LIBS raikiri::venue_binance)
target_compile_definitions(binance_wire_test PRIVATE
  RAIKIRI_BINANCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/binance/fixtures")

raikiri_add_test(bybit_book_sync_test SRCS venues/bybit/book_sync_test.cpp LIBS raikiri::venue_bybit)
raikiri_add_test(bybit_map_test SRCS venues/bybit/map_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(bybit_rest_test SRCS venues/bybit/rest_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_ws_test SRCS venues/bybit/ws_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_venue_test SRCS venues/bybit/venue_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(okx_map_test SRCS venues/okx/map_test.cpp LIBS raikiri::venue_okx raikiri::exec)

raikiri_add_test(okx_wire_test SRCS venues/okx/wire_test.cpp LIBS raikiri::venue_okx)
target_compile_definitions(okx_wire_test PRIVATE
  RAIKIRI_OKX_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/okx/fixtures")

raikiri_add_test(bybit_wire_test SRCS venues/bybit/wire_test.cpp LIBS raikiri::venue_bybit)
target_compile_definitions(bybit_wire_test PRIVATE
  RAIKIRI_BYBIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/bybit/fixtures")

raikiri_add_test(ws_client_test SRCS net/ws_client_test.cpp LIBS raikiri::net)
target_link_libraries(ws_client_test
  PRIVATE Boost::headers OpenSSL::Crypto OpenSSL::SSL)

raikiri_add_test(rest_client_test SRCS net/rest_client_test.cpp LIBS raikiri::net)
target_link_libraries(rest_client_test
  PRIVATE Boost::headers OpenSSL::Crypto OpenSSL::SSL)

raikiri_add_test(venue_contract_test SRCS exec/venue_contract_test.cpp LIBS raikiri::exec)
raikiri_add_test(reconcile_test SRCS portfolio/reconcile_test.cpp LIBS raikiri::portfolio)

raikiri_add_test(attribution_test SRCS oms/attribution_test.cpp LIBS raikiri::oms)
raikiri_add_test(funding_identity_test SRCS oms/funding_identity_test.cpp LIBS raikiri::oms)
raikiri_add_test(funding_split_test SRCS oms/funding_split_test.cpp LIBS raikiri::oms)
raikiri_add_test(recon_test SRCS oms/recon_test.cpp LIBS raikiri::oms)
raikiri_add_test(fee_ledger_test SRCS oms/fee_ledger_test.cpp LIBS raikiri::oms)
raikiri_add_test(order_store_test SRCS oms/order_store_test.cpp LIBS raikiri::oms)
raikiri_add_test(oms_risk_test SRCS oms/risk_test.cpp LIBS raikiri::oms)

raikiri_add_test(pnl_test SRCS portfolio/pnl_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(position_test SRCS portfolio/position_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(position_book_test SRCS portfolio/position_book_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(target_test SRCS portfolio/target_test.cpp LIBS raikiri::portfolio)

raikiri_add_test(spsc_ring_test SRCS shm/spsc_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(seqlock_test SRCS shm/seqlock_test.cpp LIBS raikiri::shm)
raikiri_add_test(broadcast_ring_test SRCS shm/broadcast_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(segment_test SRCS shm/segment_test.cpp LIBS raikiri::shm)
raikiri_add_test(balances_test SRCS shm/balances_test.cpp LIBS raikiri::shm)
raikiri_add_test(positions_test SRCS shm/positions_test.cpp LIBS raikiri::shm)
raikiri_add_test(catalogue_test SRCS shm/catalogue_test.cpp LIBS raikiri::shm)

raikiri_add_test(sim_book_test SRCS simcore/book_test.cpp LIBS raikiri::simcore)
raikiri_add_test(sim_engine_test SRCS simcore/engine_test.cpp LIBS raikiri::simcore)

raikiri_add_test(history_test SRCS admin/history_test.cpp LIBS raikiri::admin glaze::glaze)
raikiri_add_test(admin_stream_test SRCS admin/stream_test.cpp LIBS raikiri::admin)
