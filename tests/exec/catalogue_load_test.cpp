#include <raikiri/exec/instrument_source.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace raikiri::exec {
namespace {

using std::chrono::milliseconds;

class ScriptedSource final : public InstrumentSource {
public:
    ScriptedSource(int failures, ErrorClass cls) : failures_{failures}, cls_{cls} {}

    std::expected<std::vector<Instrument>, VenueError> fetch_all(MarketType) override {
        ++calls;
        if (calls <= failures_) {
            return std::unexpected{VenueError::make(cls_, 0, "socket closed due to a timeout")};
        }
        return std::vector<Instrument>{SpotInstrument{}};
    }

    int calls{0};

private:
    int failures_;
    ErrorClass cls_;
};

constexpr CatalogueBackoff k_fast{.first = milliseconds{1}, .cap = milliseconds{4}};

TEST(LoadCatalogue, RetriesTransientFailuresUntilLoaded) {
    ScriptedSource source{2, ErrorClass::Connectivity};
    std::vector<std::uint32_t> attempts;
    const auto loaded = load_catalogue(
        source,
        MarketType::Spot,
        [] { return false; },
        [&](const VenueError&, std::uint32_t attempt, milliseconds) { attempts.push_back(attempt); },
        k_fast);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), 1U);
    EXPECT_EQ(source.calls, 3);
    EXPECT_EQ(attempts, (std::vector<std::uint32_t>{1, 2}));
}

TEST(LoadCatalogue, GivesUpOnErrorsNoRetryCanFix) {
    for (const auto cls : {ErrorClass::Terminal, ErrorClass::AuthFailure}) {
        ScriptedSource source{1, cls};
        int retries = 0;
        const auto loaded = load_catalogue(
            source,
            MarketType::Spot,
            [] { return false; },
            [&](const VenueError&, std::uint32_t, milliseconds) { ++retries; },
            k_fast);
        ASSERT_FALSE(loaded.has_value());
        EXPECT_EQ(loaded.error().cls, cls);
        EXPECT_EQ(source.calls, 1);
        EXPECT_EQ(retries, 0);
    }
}

TEST(LoadCatalogue, StopDuringBackoffReturnsPromptly) {
    ScriptedSource source{1'000, ErrorClass::Retryable};
    std::atomic<bool> stop{false};
    const auto began = std::chrono::steady_clock::now();
    const auto loaded = load_catalogue(
        source,
        MarketType::Perp,
        [&] { return stop.load(); },
        [&](const VenueError&, std::uint32_t, milliseconds) { stop = true; },
        {.first = milliseconds{60'000}, .cap = milliseconds{60'000}});
    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(source.calls, 1);
    EXPECT_LT(std::chrono::steady_clock::now() - began, milliseconds{1'000});
}

TEST(LoadCatalogue, BackoffDoublesToTheCapWithJitter) {
    ScriptedSource source{6, ErrorClass::RateLimited};
    std::vector<milliseconds> waits;
    const auto loaded = load_catalogue(
        source,
        MarketType::Spot,
        [] { return false; },
        [&](const VenueError&, std::uint32_t, milliseconds wait) { waits.push_back(wait); },
        {.first = milliseconds{2}, .cap = milliseconds{8}});
    ASSERT_TRUE(loaded.has_value());
    const std::vector<milliseconds> ceilings{
        milliseconds{2}, milliseconds{4}, milliseconds{8}, milliseconds{8}, milliseconds{8}, milliseconds{8}};
    ASSERT_EQ(waits.size(), ceilings.size());
    for (std::size_t i = 0; i < waits.size(); ++i) {
        EXPECT_GE(waits[i], ceilings[i] / 2) << i;
        EXPECT_LE(waits[i], ceilings[i]) << i;
    }
}

} // namespace
} // namespace raikiri::exec
