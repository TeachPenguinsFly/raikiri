#include <raikiri/exec/instrument_source.hpp>

#include <raikiri/common/abandon.hpp>

#include <algorithm>
#include <random>
#include <thread>

namespace raikiri::exec {

std::expected<std::vector<Instrument>, VenueError> load_catalogue(
    InstrumentSource& source,
    MarketType market,
    const std::function<bool()>& stopping,
    const std::function<void(const VenueError&, std::uint32_t, std::chrono::milliseconds)>&
        on_retry,
    CatalogueBackoff backoff) {
    using std::chrono::milliseconds;
    using std::chrono::steady_clock;
    const AbandonScope abandon{stopping};
    std::minstd_rand rng{std::random_device{}()};
    auto delay = backoff.first;
    for (std::uint32_t attempt = 1;; ++attempt) {
        auto loaded = source.fetch_all(market);
        if (loaded.has_value()) {
            return loaded;
        }
        const auto cls = loaded.error().cls;
        if (cls == ErrorClass::Terminal || cls == ErrorClass::AuthFailure || stopping()) {
            return loaded;
        }
        std::uniform_int_distribution<milliseconds::rep> jitter{delay.count() / 2, delay.count()};
        const milliseconds wait{jitter(rng)};
        on_retry(loaded.error(), attempt, wait);
        const auto until = steady_clock::now() + wait;
        for (auto now = steady_clock::now(); now < until; now = steady_clock::now()) {
            if (stopping()) {
                return loaded;
            }
            std::this_thread::sleep_for(
                std::min<steady_clock::duration>(milliseconds{100}, until - now));
        }
        delay = std::min(delay * 2, backoff.cap);
    }
}

} // namespace raikiri::exec
