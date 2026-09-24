#include <raikiri/net/io_context.hpp>

#include <boost/asio/io_context.hpp>
#include <chrono>

namespace raikiri::net {

// only the reactor; tls contexts are per client because verification policy differs per endpoint
struct IoContext::Impl {
    boost::asio::io_context ioc{1};
    bool pumping{false};
};

IoContext::IoContext() : impl_{std::make_unique<Impl>()} {}

IoContext::~IoContext() = default;

// a ceiling on one poll so a busy socket cannot hold the loop; the rest runs next pass
constexpr std::size_t k_max_handlers_per_poll = 256;

// set while a handler may be running, including when one throws
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
        // a handler threw; what already ran still counts
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

void* IoContext::native() noexcept {
    return &impl_->ioc;
}

} // namespace raikiri::net
