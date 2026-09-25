#include <raikiri/common/abandon.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_await.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace raikiri::net {

namespace {

// on the heap so the completion can outlive the caller
struct Landing {
    std::optional<RestResponse> response;
};

} // namespace

std::optional<RestResponse> await_response(
    IoContext& io, const Clock& clock, RestClient& rest, const RestRequest& request, std::uint64_t timeout_ms) {
    // refused inside a handler: pumping a running reactor re-enters other handlers
    if (io.pumping() || AbandonScope::abandoned()) {
        return std::nullopt;
    }
    auto landing = std::make_shared<Landing>();
    // the callback captures the cell by value and keeps it alive while the pool holds it
    const auto deadline = clock.mono().ns + timeout_ms * 1'000'000ULL;
    // a refused send waits for room rather than failing
    while (!rest.send(request, [landing](RestResponse&& response) { landing->response = std::move(response); })) {
        if (clock.mono().ns >= deadline || AbandonScope::abandoned()) {
            return std::nullopt;
        }
        if (io.poll() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    while (!landing->response.has_value() && clock.mono().ns < deadline && !AbandonScope::abandoned()) {
        if (io.poll() == 0) {
            io.run_for(5);
        }
    }
    return std::move(landing->response);
}

} // namespace raikiri::net
