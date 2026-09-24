#pragma once

#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_source.hpp>

#include <memory>

#include "state.hpp"

namespace raikiri::venues::binance::internal {

[[nodiscard]] std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state);

[[nodiscard]] std::unique_ptr<exec::ExecutionClient>
make_execution_client(std::shared_ptr<VenueState> state);

[[nodiscard]] std::unique_ptr<exec::InstrumentSource>
make_instrument_source(std::shared_ptr<VenueState> state);

} // namespace raikiri::venues::binance::internal
