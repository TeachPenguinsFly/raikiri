#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace raikiri::net {

namespace {

// On the heap so the completion can outlive the caller that registered it. A
// pointer to a local here is what killed six gateways on 2026-09-10: the pull
// gave up, returned, and the answer was written into a frame the next pull was
// already using.
struct Landing {
    std::optional<RestResponse> response;
};

} // namespace

std::optional<RestResponse> await_response(IoContext& io,
                                           const Clock& clock,
                                           RestClient& rest,
                                           const RestRequest& request,
                                           std::uint64_t timeout_ms) {
    auto landing = std::make_shared<Landing>();
    // BY VALUE. Capturing the shared_ptr is the whole safety property: the
    // callback keeps the cell alive for as long as the pool keeps the callback,
    // so a delivery at any later moment writes somewhere harmless.
    if (!rest.send(request, [landing](RestResponse&& response) {
            landing->response = std::move(response);
        })) {
        return std::nullopt;
    }
    const auto deadline = clock.mono().ns + timeout_ms * 1'000'000ULL;
    while (!landing->response.has_value() && clock.mono().ns < deadline) {
        if (io.poll() == 0) {
            io.run_for(5);
        }
    }
    return std::move(landing->response);
}

} // namespace raikiri::net
