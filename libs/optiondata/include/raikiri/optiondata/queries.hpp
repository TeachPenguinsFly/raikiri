#pragma once

#include <raikiri/optiondata/board.hpp>

#include <expected>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::optiondata {

class OptionMarket {
public:
    OptionMarket() = default;
    OptionMarket(const OptionMarket&) = delete;
    OptionMarket& operator=(const OptionMarket&) = delete;
    virtual ~OptionMarket() = default;

    [[nodiscard]] virtual std::vector<UnderlyingListing> underlyings() const = 0;

    // replaces `out` with the underlying's listings, of one expiry when given, and their latest market state
    virtual void
    read(std::string_view underlying, std::optional<UnixNanos> expiry, std::vector<OptionQuote>& out) const = 0;
};

using WallClock = std::function<UnixNanos()>;

class ListUnderlyings {
public:
    explicit ListUnderlyings(const OptionMarket& market) : market_{&market} {}

    [[nodiscard]] std::vector<UnderlyingListing> operator()() const { return market_->underlyings(); }

private:
    const OptionMarket* market_;
};

// `expiry` is the epoch instant or a utc date, YYYYMMDD, which takes that date's first listed expiry
[[nodiscard]] std::optional<UnixNanos> resolve_expiry(const UnderlyingListing& listing,
                                                      std::string_view expiry) noexcept;

struct ChainRequest {
    std::string_view underlying{};
    std::string_view expiry{};
    std::vector<VenueId> venues{}; // empty is all
};

enum class ChainError : std::uint8_t { NoUnderlying = 1, NoExpiry };

class GetChain {
public:
    GetChain(const OptionMarket& market, WallClock now) : market_{&market}, now_{std::move(now)} {}

    [[nodiscard]] std::expected<ChainView, ChainError> operator()(const ChainRequest& request) const;

private:
    const OptionMarket* market_;
    WallClock now_;
    mutable std::vector<OptionQuote> quotes_;
};

inline constexpr std::uint64_t k_surface_refresh_ns = 3'000'000'000ULL;

enum class Refit : std::uint8_t { Inline = 0, Background };

// a fit takes milliseconds to seconds, so one per underlying and venue set is kept for `refresh_ns`; in the
// background the stale fit answers while the next is fitted on its own thread from quotes read here
class GetSurface {
public:
    GetSurface(const OptionMarket& market,
               WallClock now,
               Refit refit = Refit::Background,
               std::uint64_t refresh_ns = k_surface_refresh_ns)
        : market_{&market}, now_{std::move(now)}, refit_{refit}, refresh_ns_{refresh_ns} {}

    [[nodiscard]] std::optional<SurfaceView> operator()(std::string_view underlying, std::span<const VenueId> venues);

    // blocks until every background fit has landed
    void settle();

private:
    struct Entry {
        std::optional<SurfaceView> view{};
        std::future<SurfaceView> pending{};
    };

    const OptionMarket* market_;
    WallClock now_;
    Refit refit_;
    std::uint64_t refresh_ns_;
    std::map<std::string, Entry, std::less<>> cache_;
};

} // namespace raikiri::optiondata
