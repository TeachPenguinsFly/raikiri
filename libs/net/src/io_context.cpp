#include <raikiri/net/io_context.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>

namespace raikiri::net {

// Only the reactor lives here. TLS contexts stay with WsClient and RestClient
// instead: verification mode, the trust store and SNI are per-endpoint policy
// (a venue's public stream and a loopback test server disagree about all
// three), and one process-wide context would force every client to share
// whichever policy was configured first.
struct IoContext::Impl {
    // Concurrency hint 1 tells Asio the reactor is never contended, which
    // drops the internal locking it would otherwise use. That is a promise we
    // keep by contract: one thread, the gateway loop, forever.
    boost::asio::io_context ioc{1};
};

IoContext::IoContext() : impl_{std::make_unique<Impl>()} {}

IoContext::~IoContext() = default;

std::size_t IoContext::poll() noexcept {
    // A context that once ran out of work latches "stopped" and from then on
    // executes nothing, silently. The symptom is a gateway that connects,
    // goes quiet, and never reconnects, because the reconnect timer's handler
    // is queued in a reactor that refuses to run it. One predicate on a path
    // that is about to enter the kernel anyway is cheap insurance.
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    try {
        return impl_->ioc.poll();
    } catch (...) {
        // poll() is noexcept by contract, so an escaping handler exception
        // would terminate the process. The clients catch at every handler
        // boundary; this is the backstop for anything that slips past, and
        // the remaining ready handlers still run on the next poll.
        return 0;
    }
}

std::size_t IoContext::run_for(std::size_t timeout_ms) noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    try {
        return impl_->ioc.run_for(
            std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(timeout_ms)});
    } catch (...) {
        return 0;
    }
}

void IoContext::stop() noexcept {
    impl_->ioc.stop();
}

void* IoContext::native() noexcept {
    return &impl_->ioc;
}

} // namespace raikiri::net
