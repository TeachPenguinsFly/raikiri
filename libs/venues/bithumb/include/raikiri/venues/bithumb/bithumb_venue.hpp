#pragma once

#include <raikiri/exec/venue.hpp>
#include <raikiri/venues/upbit/flavour.hpp>

namespace raikiri::venues::bithumb {

// the upbit v1 api from bithumb's hosts; no ticker for every market, the rest ticker is stamped
// nine hours ahead, and a top unit can arrive emptied, so quotes take five units
inline constexpr upbit::Flavour k_flavour{.venue = VenueId::Bithumb,
                                          .name = "bithumb",
                                          .rest_host = "api.bithumb.com",
                                          .ws_host = "ws-api.bithumb.com",
                                          .ws_path = "/websocket/v1",
                                          .ticker_all = {},
                                          .ticker_chunk = 100,
                                          .ticker_stamped = false,
                                          .quote_units = ".5"};

void register_adapter();

} // namespace raikiri::venues::bithumb
