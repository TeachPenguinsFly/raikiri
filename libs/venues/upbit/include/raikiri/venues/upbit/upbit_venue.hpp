#pragma once

#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/upbit/flavour.hpp>

namespace raikiri::venues::upbit {

void register_adapter();

// for a venue that serves the same v1 api from its own hosts
[[nodiscard]] exec::VenueAdapter
make_adapter(const Flavour& flavour, const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock);

} // namespace raikiri::venues::upbit
