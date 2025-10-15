#pragma once

// The cold path's blocking request/response: catalogue loads, clock syncs and
// reconciliation pulls, never the gateway loop. Pumping the reactor until the
// answer arrives is right for those — a gateway that trades before it knows
// its tick sizes is worse than one that takes an extra second to boot.
//
// ONE COPY, deliberately. Every venue used to spell this out in its own
// private state.hpp, byte for byte, so a lifetime defect in it was a lifetime
// defect seventeen times over.

#include <raikiri/common/time.hpp>
#include <raikiri/net/rest_client.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::net {

class IoContext;

// Pumps `io` until the answer arrives, or until `timeout_ms` passes with none.
//
// THE COMPLETION MUST NEVER BE ABLE TO WRITE INTO THIS FRAME. Giving up is not
// cancelling: the pool still holds the callback, and delivers it whenever the
// request finally resolves — which for a stalled name lookup is minutes later,
// into a stack the next pull is already using. So the answer lands in a heap
// cell the callback co-owns rather than in a local: a late delivery writes into
// a cell only the dead callback still points at, and the cell dies with it.
//
// `timeout_ms` MUST EXCEED RestClient::max_wait_ms(), or this deadline races
// the transport's own and the half where this one wins calls a venue silent
// while the pool still holds its request.
[[nodiscard]] std::optional<RestResponse> await_response(IoContext& io,
                                                         const Clock& clock,
                                                         RestClient& rest,
                                                         const RestRequest& request,
                                                         std::uint64_t timeout_ms);

} // namespace raikiri::net
