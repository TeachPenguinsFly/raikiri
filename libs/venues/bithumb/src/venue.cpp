#include <raikiri/venues/bithumb/bithumb_venue.hpp>
#include <raikiri/venues/upbit/upbit_venue.hpp>

namespace raikiri::venues::bithumb {

namespace {

exec::VenueAdapter make(const exec::VenueConfig& config, exec::GatewaySink& sink, const Clock& clock) {
    return upbit::make_adapter(k_flavour, config, sink, clock);
}

} // namespace

void register_adapter() {
    exec::register_venue(VenueId::Bithumb, &make);
}

} // namespace raikiri::venues::bithumb
