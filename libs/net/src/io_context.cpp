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
    bool pumping{false};
};

IoContext::IoContext() : impl_{std::make_unique<Impl>()} {}

IoContext::~IoContext() = default;

// A CEILING ON ONE CALL, which asio's own poll() has none of: it runs handlers
// until none is ready, and a busy socket makes another one ready as it goes, so
// on a venue delivering thousands of frames a second the call simply does not
// come back. Measured on bingx's full catalogue at 4800 messages a second, a
// gateway asked to stop after 60 s ran for 153, because the deadline is read at
// the top of a loop this call was holding.
//
// The header above promises that no venue can delay a cancel. It is this bound
// that keeps the promise: everything else in the loop — the command rings, the
// pool's paced sends, the deadline — runs between two batches, not after the
// venue goes quiet. Nothing is dropped; what is not run now is run on the next
// pass, which is the very next thing the caller does.
constexpr std::size_t k_max_handlers_per_poll = 256;

// Set for exactly as long as a handler may be running, including when one of
// them throws.
struct Pumping {
    explicit Pumping(bool& flag) noexcept : flag_{flag} { flag_ = true; }
    ~Pumping() { flag_ = false; }
    Pumping(const Pumping&) = delete;
    Pumping& operator=(const Pumping&) = delete;

    bool& flag_;
};

std::size_t IoContext::poll() noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    const Pumping guard{impl_->pumping};
    std::size_t ran = 0;
    try {
        while (ran < k_max_handlers_per_poll) {
            const auto one = impl_->ioc.poll_one();
            if (one == 0) {
                break;
            }
            ran += one;
        }
    } catch (...) {
        // A handler threw. What already ran still ran, and saying so is the
        // honest count.
    }
    return ran;
}

std::size_t IoContext::run_for(std::size_t timeout_ms) noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    const Pumping guard{impl_->pumping};
    try {
        return impl_->ioc.run_for(
            std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(timeout_ms)});
    } catch (...) {
        return 0;
    }
}

bool IoContext::pumping() const noexcept {
    return impl_->pumping;
}

void IoContext::stop() noexcept {
    impl_->ioc.stop();
}

std::size_t IoContext::run_one_for(std::size_t timeout_ms) noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    const Pumping guard{impl_->pumping};
    try {
        return impl_->ioc.run_one_for(
            std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(timeout_ms)});
    } catch (...) {
        return 0;
    }
}

void* IoContext::native() noexcept {
    return &impl_->ioc;
}

} // namespace raikiri::net
