#pragma once

// blocking request/response for cold paths only: catalogue loads, clock syncs, reconciliation pulls

#include <raikiri/common/time.hpp>
#include <raikiri/net/rest_client.hpp>

#include <cstdint>
#include <optional>

namespace raikiri::net {

class IoContext;

// pumps `io` until an answer, `timeout_ms` or an AbandonScope; the answer lands in a heap cell the callback co-owns
// `timeout_ms` must exceed RestClient::max_wait_ms()
[[nodiscard]] std::optional<RestResponse> await_response(
    IoContext& io, const Clock& clock, RestClient& rest, const RestRequest& request, std::uint64_t timeout_ms);

} // namespace raikiri::net
