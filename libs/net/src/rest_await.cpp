#include <raikiri/common/abandon.hpp>
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
    // NOT FROM INSIDE A HANDLER. The loop below pumps the reactor, and pumping
    // one that is already running executes OTHER handlers — re-entering state
    // the caller above is halfway through changing. asio documents the same
    // hazard for nested run(); poll() merely makes it quiet instead of
    // deadlocking, which is worse.
    //
    // Refused rather than asserted, because the caller already handles "no
    // answer" and an assert in a release build is no guard at all. A caller
    // that reaches this is a caller that must be made asynchronous.
    if (io.pumping() || AbandonScope::abandoned()) {
        return std::nullopt;
    }
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
    while (!landing->response.has_value() && clock.mono().ns < deadline &&
           !AbandonScope::abandoned()) {
        if (io.poll() == 0) {
            io.run_for(5);
        }
    }
    return std::move(landing->response);
}

} // namespace raikiri::net
