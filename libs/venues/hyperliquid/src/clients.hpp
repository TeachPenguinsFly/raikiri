#pragma once

// Each client is defined privately in its own translation unit; only these
// three functions cross the boundary.

#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/execution_client.hpp>
#include <raikiri/exec/instrument_source.hpp>

#include <memory>

#include "state.hpp"

namespace raikiri::venues::hyperliquid::internal {

[[nodiscard]] std::unique_ptr<exec::DataClient> make_data_client(std::shared_ptr<VenueState> state);

[[nodiscard]] std::unique_ptr<exec::ExecutionClient>
make_execution_client(std::shared_ptr<VenueState> state);

[[nodiscard]] std::unique_ptr<exec::InstrumentSource>
make_instrument_source(std::shared_ptr<VenueState> state);

} // namespace raikiri::venues::hyperliquid::internal
