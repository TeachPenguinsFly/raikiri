#include <raikiri/exec/caps.hpp>
#include <raikiri/venues/bitunix/bitunix_venue.hpp>

#include <memory>

#include "clients.hpp"

namespace raikiri::venues::bitunix {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    auto state = std::make_shared<internal::VenueState>(sink, clock, config.market);
    state->budget = config.budget;
    state->order_answer_timeout_ns = config.order_answer_timeout_ns;

    exec::VenueAdapter adapter{};
    adapter.caps = exec::caps_of(VenueId::Bitunix);
    adapter.data = internal::make_data_client(state);
    adapter.execution = internal::make_execution_client(state);
    adapter.instruments = internal::make_instrument_source(state);
    return adapter;
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bitunix, &make);
}

} // namespace raikiri::venues::bitunix
