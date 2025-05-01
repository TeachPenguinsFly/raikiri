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
    boost::asio::io_context ioc{1};
};

IoContext::IoContext() : impl_{std::make_unique<Impl>()} {}

IoContext::~IoContext() = default;

std::size_t IoContext::poll() noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    try {
        return impl_->ioc.poll();
    } catch (...) {
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
