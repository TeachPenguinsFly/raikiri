include(GoogleTest)

function(raikiri_add_test name)
  cmake_parse_arguments(ARG "POSTGRES" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name}
    PRIVATE ${ARG_LIBS} GTest::gtest_main raikiri::warnings)
  if(ARG_POSTGRES)
    # run one at a time; concurrent migration ddl deadlocks
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30 PROPERTIES RESOURCE_LOCK postgres)
  else()
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
  endif()
endfunction()

raikiri_add_test(histogram_test SRCS observ/histogram_test.cpp LIBS raikiri::observ)

raikiri_add_test(fixed_point_test SRCS common/fixed_point_test.cpp LIBS raikiri::common)
raikiri_add_test(fixed_string_test SRCS common/fixed_string_test.cpp LIBS raikiri::common)
raikiri_add_test(ulid_test SRCS common/ulid_test.cpp LIBS raikiri::common)
raikiri_add_test(platform_test SRCS common/platform_test.cpp LIBS raikiri::common)
raikiri_add_test(time_test SRCS common/time_test.cpp LIBS raikiri::common)

raikiri_add_test(identifiers_test SRCS domain/identifiers_test.cpp LIBS raikiri::domain)
raikiri_add_test(instrument_test SRCS domain/instrument_test.cpp LIBS raikiri::domain)
raikiri_add_test(asset_class_test SRCS domain/asset_class_test.cpp LIBS raikiri::domain)
raikiri_add_test(fill_identity_test SRCS domain/fill_identity_test.cpp LIBS raikiri::domain)
raikiri_add_test(symbol_test SRCS domain/symbol_test.cpp LIBS raikiri::domain)
raikiri_add_test(order_fsm_test SRCS domain/order_fsm_test.cpp LIBS raikiri::domain)

raikiri_add_test(journal_test SRCS journal/journal_test.cpp LIBS raikiri::journal raikiri::schema raikiri::domain)

raikiri_add_test(mapper_test POSTGRES SRCS persist/mapper_test.cpp LIBS raikiri::persist raikiri::schema raikiri::oms libpqxx::pqxx)
target_compile_definitions(mapper_test PRIVATE RAIKIRI_SQL_DIR="${PROJECT_SOURCE_DIR}/sql")

raikiri_add_test(credentials_test SRCS config/credentials_test.cpp LIBS raikiri::config)
raikiri_add_test(secret_test SRCS config/secret_test.cpp LIBS raikiri::config)

raikiri_add_test(json_writer_test SRCS net/json_writer_test.cpp LIBS raikiri::net)
raikiri_add_test(protobuf_test SRCS wire/protobuf_test.cpp LIBS raikiri::wire)
raikiri_add_test(unescape_test SRCS wire/unescape_test.cpp LIBS raikiri::wire raikiri::wire_malformed)

set(malformed_srcs wire/malformed/wire.cpp)
set(malformed_libs raikiri::wire raikiri::wire_malformed)
foreach(venue IN ITEMS apex aster backpack binance bingx bitfinex bitget blofin bybit coinbase
    cryptocom deribit dydx edgex extended gate grvt htx hyperliquid kraken kucoin lighter mexc
    lbank okx pacifica phemex weex xt bitunix toobit derive arcus ostium paradex vest bullish
    ondoperps polymarketperps upbit bithumb bitstamp nado)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/wire/malformed/${venue}.cpp)
    list(APPEND malformed_srcs wire/malformed/${venue}.cpp)
    list(APPEND malformed_libs raikiri::venue_${venue})
  endif()
endforeach()
raikiri_add_test(wire_malformed_test SRCS ${malformed_srcs} LIBS ${malformed_libs})
target_compile_definitions(wire_malformed_test PRIVATE
  RAIKIRI_VENUE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues")

raikiri_add_test(admin_test SRCS admin/admin_test.cpp LIBS raikiri::admin)
target_link_libraries(admin_test
  PRIVATE glaze::glaze)

raikiri_add_test(gatewayd_private_pulls_test SRCS gatewayd/private_pulls_test.cpp LIBS raikiri::domain)
target_include_directories(gatewayd_private_pulls_test PRIVATE ${PROJECT_SOURCE_DIR}/apps/gatewayd)

raikiri_add_test(settings_test SRCS config/settings_test.cpp LIBS raikiri::config raikiri::exec)
# uses the committed config files so a file the shipped loader cannot read fails here
target_compile_definitions(settings_test PRIVATE RAIKIRI_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")

raikiri_add_test(signer_test SRCS net/signer_test.cpp LIBS raikiri::net)

raikiri_add_test(gate_venue_test SRCS venues/gate/venue_test.cpp LIBS raikiri::venue_gate raikiri::exec)
raikiri_add_test(gate_option_test SRCS venues/gate/option_test.cpp LIBS raikiri::venue_gate raikiri::options raikiri::exec)
raikiri_add_test(gate_trade_test SRCS venues/gate/trade_test.cpp LIBS raikiri::venue_gate raikiri::net raikiri::exec)
raikiri_add_test(kucoin_venue_test SRCS venues/kucoin/venue_test.cpp LIBS raikiri::venue_kucoin raikiri::exec)
raikiri_add_test(kucoin_trade_test SRCS venues/kucoin/trade_test.cpp LIBS raikiri::venue_kucoin raikiri::exec raikiri::net)
raikiri_add_test(bingx_venue_test SRCS venues/bingx/venue_test.cpp LIBS raikiri::venue_bingx raikiri::exec)
raikiri_add_test(xt_venue_test SRCS venues/xt/venue_test.cpp LIBS raikiri::venue_xt raikiri::exec)
raikiri_add_test(mexc_venue_test SRCS venues/mexc/venue_test.cpp LIBS raikiri::venue_mexc raikiri::exec)
raikiri_add_test(deribit_venue_test SRCS venues/deribit/venue_test.cpp LIBS raikiri::venue_deribit raikiri::exec)
raikiri_add_test(options_test SRCS options/options_test.cpp LIBS raikiri::options)
raikiri_add_test(pricing_test SRCS options/pricing_test.cpp LIBS raikiri::options raikiri::lets_be_rational)
raikiri_add_test(surface_test SRCS options/surface_test.cpp LIBS raikiri::options raikiri::wire)
target_compile_definitions(surface_test PRIVATE RAIKIRI_SURFACE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/options/fixtures")
raikiri_add_test(hyperliquid_venue_test SRCS venues/hyperliquid/venue_test.cpp LIBS raikiri::venue_hyperliquid raikiri::exec)
raikiri_add_test(hyperliquid_post_test SRCS venues/hyperliquid/post_test.cpp LIBS raikiri::venue_hyperliquid raikiri::net)
target_compile_definitions(hyperliquid_post_test PRIVATE
  RAIKIRI_HYPERLIQUID_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/hyperliquid/fixtures")
raikiri_add_test(coinbase_venue_test SRCS venues/coinbase/venue_test.cpp LIBS raikiri::venue_coinbase raikiri::exec)
raikiri_add_test(kraken_venue_test SRCS venues/kraken/venue_test.cpp LIBS raikiri::venue_kraken raikiri::exec)
raikiri_add_test(kraken_spot_test SRCS venues/kraken/spot_test.cpp LIBS raikiri::venue_kraken raikiri::exec)
raikiri_add_test(bitget_venue_test SRCS venues/bitget/venue_test.cpp LIBS raikiri::venue_bitget raikiri::exec)
raikiri_add_test(bitget_trade_test SRCS venues/bitget/trade_test.cpp LIBS raikiri::venue_bitget raikiri::exec raikiri::net)
raikiri_add_test(htx_venue_test SRCS venues/htx/venue_test.cpp LIBS raikiri::venue_htx raikiri::exec)
raikiri_add_test(bitfinex_venue_test SRCS venues/bitfinex/venue_test.cpp LIBS raikiri::venue_bitfinex raikiri::exec)
raikiri_add_test(weex_venue_test SRCS venues/weex/venue_test.cpp LIBS raikiri::venue_weex raikiri::exec)
raikiri_add_test(weex_spot_exec_test SRCS venues/weex/spot_exec_test.cpp LIBS raikiri::venue_weex raikiri::exec)
raikiri_add_test(ladder_test SRCS book/ladder_test.cpp LIBS raikiri::book)
raikiri_add_test(order_gate_test SRCS exec/order_gate_test.cpp LIBS raikiri::exec)
raikiri_add_test(catalogue_load_test SRCS exec/catalogue_load_test.cpp LIBS raikiri::exec)
raikiri_add_test(rate_budget_test SRCS exec/rate_budget_test.cpp LIBS raikiri::exec)
raikiri_add_test(aster_venue_test SRCS venues/aster/venue_test.cpp LIBS raikiri::venue_aster raikiri::exec)
raikiri_add_test(grvt_venue_test SRCS venues/grvt/venue_test.cpp LIBS raikiri::venue_grvt raikiri::exec)
raikiri_add_test(lighter_venue_test SRCS venues/lighter/venue_test.cpp LIBS raikiri::venue_lighter raikiri::exec)
raikiri_add_test(backpack_venue_test SRCS venues/backpack/venue_test.cpp LIBS raikiri::venue_backpack raikiri::exec)
raikiri_add_test(cryptocom_venue_test SRCS venues/cryptocom/venue_test.cpp LIBS raikiri::venue_cryptocom raikiri::exec)
raikiri_add_test(blofin_venue_test SRCS venues/blofin/venue_test.cpp LIBS raikiri::venue_blofin raikiri::exec)
raikiri_add_test(apex_venue_test SRCS venues/apex/venue_test.cpp LIBS raikiri::venue_apex raikiri::exec)
raikiri_add_test(edgex_venue_test SRCS venues/edgex/venue_test.cpp LIBS raikiri::venue_edgex raikiri::exec)
raikiri_add_test(dydx_venue_test SRCS venues/dydx/venue_test.cpp LIBS raikiri::venue_dydx raikiri::exec)
raikiri_add_test(phemex_venue_test SRCS venues/phemex/venue_test.cpp LIBS raikiri::venue_phemex raikiri::exec)
raikiri_add_test(extended_venue_test SRCS venues/extended/venue_test.cpp LIBS raikiri::venue_extended raikiri::exec)
raikiri_add_test(pacifica_venue_test SRCS venues/pacifica/venue_test.cpp LIBS raikiri::venue_pacifica raikiri::exec)
raikiri_add_test(lbank_venue_test SRCS venues/lbank/venue_test.cpp LIBS raikiri::venue_lbank raikiri::exec)
raikiri_add_test(bitunix_venue_test SRCS venues/bitunix/venue_test.cpp LIBS raikiri::venue_bitunix raikiri::exec)
raikiri_add_test(toobit_venue_test SRCS venues/toobit/venue_test.cpp LIBS raikiri::venue_toobit raikiri::exec)
raikiri_add_test(arcus_venue_test SRCS venues/arcus/venue_test.cpp LIBS raikiri::venue_arcus raikiri::exec)
raikiri_add_test(paradex_venue_test SRCS venues/paradex/venue_test.cpp LIBS raikiri::venue_paradex raikiri::exec)
raikiri_add_test(bullish_venue_test SRCS venues/bullish/venue_test.cpp LIBS raikiri::venue_bullish raikiri::exec)
raikiri_add_test(upbit_venue_test SRCS venues/upbit/venue_test.cpp LIBS raikiri::venue_upbit raikiri::exec)
raikiri_add_test(bithumb_venue_test SRCS venues/bithumb/venue_test.cpp LIBS raikiri::venue_bithumb raikiri::exec raikiri::venue_upbit)
raikiri_add_test(bitstamp_venue_test SRCS venues/bitstamp/venue_test.cpp LIBS raikiri::venue_bitstamp raikiri::exec)
raikiri_add_test(nado_venue_test SRCS venues/nado/venue_test.cpp LIBS raikiri::venue_nado raikiri::exec)
raikiri_add_test(derive_venue_test SRCS venues/derive/venue_test.cpp LIBS raikiri::venue_derive raikiri::options raikiri::exec)
raikiri_add_test(ostium_venue_test SRCS venues/ostium/venue_test.cpp LIBS raikiri::venue_ostium raikiri::exec)
raikiri_add_test(vest_venue_test SRCS venues/vest/venue_test.cpp LIBS raikiri::venue_vest raikiri::exec)
raikiri_add_test(ondoperps_venue_test SRCS venues/ondoperps/venue_test.cpp LIBS raikiri::venue_ondoperps raikiri::exec)
raikiri_add_test(polymarketperps_venue_test SRCS venues/polymarketperps/venue_test.cpp LIBS raikiri::venue_polymarketperps raikiri::exec)
raikiri_add_test(binance_map_test SRCS venues/binance/map_test.cpp LIBS raikiri::venue_binance raikiri::exec)
raikiri_add_test(binance_option_test SRCS venues/binance/option_test.cpp LIBS raikiri::venue_binance raikiri::options raikiri::exec raikiri::net)
target_compile_definitions(binance_option_test PRIVATE
  RAIKIRI_BINANCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/binance/fixtures")
raikiri_add_test(binance_rest_test SRCS venues/binance/rest_test.cpp LIBS raikiri::venue_binance raikiri::net)

raikiri_add_test(binance_wire_test SRCS venues/binance/wire_test.cpp LIBS raikiri::venue_binance)
target_compile_definitions(binance_wire_test PRIVATE
  RAIKIRI_BINANCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/binance/fixtures")
raikiri_add_test(binance_spot_test SRCS venues/binance/spot_test.cpp LIBS raikiri::venue_binance raikiri::exec raikiri::net)
target_compile_definitions(binance_spot_test PRIVATE
  RAIKIRI_BINANCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/binance/fixtures")
raikiri_add_test(binance_ws_api_test SRCS venues/binance/ws_api_test.cpp LIBS raikiri::venue_binance raikiri::net)
target_compile_definitions(binance_ws_api_test PRIVATE
  RAIKIRI_BINANCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/binance/fixtures")

raikiri_add_test(bybit_book_sync_test SRCS venues/bybit/book_sync_test.cpp LIBS raikiri::venue_bybit)
raikiri_add_test(bybit_map_test SRCS venues/bybit/map_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(bybit_option_test SRCS venues/bybit/option_test.cpp LIBS raikiri::venue_bybit raikiri::options raikiri::exec)
raikiri_add_test(bybit_rest_test SRCS venues/bybit/rest_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_ws_test SRCS venues/bybit/ws_test.cpp LIBS raikiri::venue_bybit raikiri::net)
raikiri_add_test(bybit_trade_test SRCS venues/bybit/trade_test.cpp LIBS raikiri::venue_bybit raikiri::net)
target_compile_definitions(bybit_trade_test PRIVATE
  RAIKIRI_BYBIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/venues/bybit/fixtures")
raikiri_add_test(bybit_venue_test SRCS venues/bybit/venue_test.cpp LIBS raikiri::venue_bybit raikiri::exec)
raikiri_add_test(okx_map_test SRCS venues/okx/map_test.cpp LIBS raikiri::venue_okx raikiri::exec)
raikiri_add_test(okx_option_test SRCS venues/okx/option_test.cpp LIBS raikiri::venue_okx raikiri::options raikiri::exec)

raikiri_add_test(okx_trade_test SRCS venues/okx/trade_test.cpp LIBS raikiri::venue_okx raikiri::net raikiri::exec)
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
raikiri_add_test(ws_pool_test SRCS net/ws_pool_test.cpp LIBS raikiri::net)
raikiri_add_test(ws_backoff_test SRCS net/ws_backoff_test.cpp LIBS raikiri::net)
raikiri_add_test(ws_rpc_test SRCS net/ws_rpc_test.cpp LIBS raikiri::net raikiri::exec)
target_link_libraries(rest_client_test
  PRIVATE Boost::headers OpenSSL::Crypto OpenSSL::SSL)

raikiri_add_test(rest_await_test SRCS net/rest_await_test.cpp LIBS raikiri::net raikiri::exec)
target_link_libraries(rest_await_test
  PRIVATE Boost::headers OpenSSL::Crypto OpenSSL::SSL)

raikiri_add_test(venue_contract_test SRCS exec/venue_contract_test.cpp LIBS raikiri::exec)
raikiri_add_test(venue_clock_test SRCS exec/venue_clock_test.cpp LIBS raikiri::exec raikiri::common
  raikiri::venue_apex raikiri::venue_aster raikiri::venue_backpack raikiri::venue_binance raikiri::venue_bingx
  raikiri::venue_bitfinex raikiri::venue_bitget raikiri::venue_bitunix raikiri::venue_blofin raikiri::venue_bybit
  raikiri::venue_coinbase raikiri::venue_cryptocom raikiri::venue_deribit raikiri::venue_derive raikiri::venue_dydx
  raikiri::venue_edgex raikiri::venue_extended raikiri::venue_gate raikiri::venue_grvt raikiri::venue_htx
  raikiri::venue_hyperliquid raikiri::venue_kraken raikiri::venue_kucoin raikiri::venue_lbank raikiri::venue_lighter
  raikiri::venue_mexc raikiri::venue_okx raikiri::venue_pacifica raikiri::venue_phemex raikiri::venue_toobit
  raikiri::venue_weex raikiri::venue_xt raikiri::venue_upbit raikiri::venue_bithumb raikiri::venue_bitstamp)
target_compile_definitions(venue_clock_test PRIVATE
  RAIKIRI_SERVER_TIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/exec/fixtures/server_time")
raikiri_add_test(reconcile_test SRCS portfolio/reconcile_test.cpp LIBS raikiri::portfolio)

raikiri_add_test(attribution_test SRCS oms/attribution_test.cpp LIBS raikiri::oms)
raikiri_add_test(funding_identity_test SRCS oms/funding_identity_test.cpp LIBS raikiri::oms)
raikiri_add_test(funding_split_test SRCS oms/funding_split_test.cpp LIBS raikiri::oms)
raikiri_add_test(recon_test SRCS oms/recon_test.cpp LIBS raikiri::oms)
raikiri_add_test(kernel_gate_test SRCS oms/kernel_gate_test.cpp LIBS raikiri::oms)
raikiri_add_test(fee_ledger_test SRCS oms/fee_ledger_test.cpp LIBS raikiri::oms)
raikiri_add_test(order_store_test SRCS oms/order_store_test.cpp LIBS raikiri::oms)
raikiri_add_test(oms_risk_test SRCS oms/risk_test.cpp LIBS raikiri::oms)

raikiri_add_test(pnl_test SRCS portfolio/pnl_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(position_test SRCS portfolio/position_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(position_book_test SRCS portfolio/position_book_test.cpp LIBS raikiri::portfolio)
raikiri_add_test(target_test SRCS portfolio/target_test.cpp LIBS raikiri::portfolio)

raikiri_add_test(strategy_lifecycle_test SRCS strategy/lifecycle_test.cpp LIBS raikiri::strategy)
raikiri_add_test(strategy_registry_test SRCS strategy/registry_test.cpp
  LIBS raikiri::strategies raikiri::strategy)
raikiri_add_test(strategy_control_test SRCS strategy/control_test.cpp
  LIBS raikiri::strategy_control glaze::glaze)

raikiri_add_test(spsc_ring_test SRCS shm/spsc_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(seqlock_test SRCS shm/seqlock_test.cpp LIBS raikiri::shm)
raikiri_add_test(broadcast_ring_test SRCS shm/broadcast_ring_test.cpp LIBS raikiri::shm)
raikiri_add_test(segment_test SRCS shm/segment_test.cpp LIBS raikiri::shm)
raikiri_add_test(lease_test SRCS shm/lease_test.cpp LIBS raikiri::shm)
raikiri_add_test(balances_test SRCS shm/balances_test.cpp LIBS raikiri::shm)
raikiri_add_test(positions_test SRCS shm/positions_test.cpp LIBS raikiri::shm)
raikiri_add_test(catalogue_test SRCS shm/catalogue_test.cpp LIBS raikiri::shm)

raikiri_add_test(sim_book_test SRCS simcore/book_test.cpp LIBS raikiri::simcore)
raikiri_add_test(sim_engine_test SRCS simcore/engine_test.cpp LIBS raikiri::simcore)

raikiri_add_test(history_test POSTGRES SRCS admin/history_test.cpp LIBS raikiri::admin raikiri::persist raikiri::schema glaze::glaze libpqxx::pqxx)
raikiri_add_test(admin_stream_test SRCS admin/stream_test.cpp LIBS raikiri::admin)
raikiri_add_test(admin_stream_token_test SRCS admin/stream_token_test.cpp LIBS raikiri::admin Boost::headers)

add_test(NAME apps_help
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/apps/help_test.sh
    $<TARGET_FILE:gatewayd> $<TARGET_FILE:admind> $<TARGET_FILE:persisterd> $<TARGET_FILE:strategyd>
    $<TARGET_FILE:symbolaudit> $<TARGET_FILE:refd> $<TARGET_FILE:reftop>)
set_tests_properties(apps_help PROPERTIES TIMEOUT 30)

add_test(NAME deploy_venue_args
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/deploy/venue_args_test.sh ${CMAKE_CURRENT_SOURCE_DIR}/../deploy/venue_args.sh)
set_tests_properties(deploy_venue_args PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

raikiri_add_test(introspect_domain_test SRCS introspect/domain_test.cpp LIBS raikiri::introspect)
raikiri_add_test(introspect_files_test SRCS introspect/files_test.cpp
  LIBS raikiri::introspect_files glaze::glaze)
raikiri_add_test(introspect_routes_test SRCS introspect/routes_test.cpp
  LIBS raikiri::introspect_http glaze::glaze)
raikiri_add_test(introspect_latency_test SRCS introspect/latency_test.cpp
  LIBS raikiri::introspect_http raikiri::introspect_probe)

raikiri_add_test(joe_domain_test SRCS joe/domain_test.cpp LIBS raikiri::joe)

raikiri_add_test(reference_mid_test SRCS reference/mid_test.cpp LIBS raikiri::reference)
raikiri_add_test(reference_segment_test SRCS reference/segment_test.cpp
  LIBS raikiri::reference_shm raikiri::reference_http)
