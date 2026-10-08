#pragma once

#include <raikiri/reference/mid.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::reference {

struct AssetQuotes {
    std::string asset{}; // the asset key, "crypto:BTC"
    std::vector<Quote> quotes{};
};

inline constexpr std::string_view k_usdt_key = "crypto:USDT";
inline constexpr std::string_view k_usdc_key = "crypto:USDC";

class QuoteSource {
public:
    QuoteSource() = default;
    QuoteSource(const QuoteSource&) = delete;
    QuoteSource& operator=(const QuoteSource&) = delete;
    virtual ~QuoteSource() = default;

    // every listed spot and perp book, grouped by asset key; the stablecoins' own books included
    [[nodiscard]] virtual std::vector<AssetQuotes> snapshot() = 0;
};

struct Row {
    std::string asset{};
    Reference ref{};
};

struct CycleReport {
    std::uint64_t at_ns{0};
    std::uint64_t took_ns{0};
    std::size_t quotes{0};
    std::size_t refused{0};
    StableRates rates{};
};

class ReferencePublisher {
public:
    ReferencePublisher() = default;
    ReferencePublisher(const ReferencePublisher&) = delete;
    ReferencePublisher& operator=(const ReferencePublisher&) = delete;
    virtual ~ReferencePublisher() = default;

    virtual void publish(std::span<const Row> rows, const CycleReport& report) = 0;
};

struct Universe {
    std::vector<std::string> pinned{}; // asset keys
    std::size_t top_n{20};
    // a ranked asset needs this many venues quoting it
    std::size_t min_venues{3};
    // the mid is spot-anchored, so an asset quoted only on perps never ranks
    std::size_t min_spot_venues{2};
};

// pinned first, then the others by summed usd turnover; stablecoins never rank
[[nodiscard]] std::vector<std::string>
rank_universe(const Universe& spec, std::span<const AssetQuotes> book, const StableRates& rates);

[[nodiscard]] bool is_stablecoin(std::string_view asset) noexcept;

using WallClock = std::function<std::uint64_t()>;

inline constexpr std::uint64_t k_rerank_ns = 60ULL * 1'000'000'000ULL;

// one cycle per call; the universe only grows, so a published row never goes quiet while the process runs
class ComputeReferences {
public:
    ComputeReferences(QuoteSource& source,
                      ReferencePublisher& sink,
                      Params params,
                      Universe universe,
                      WallClock wall,
                      std::uint64_t rerank_ns = k_rerank_ns);

    CycleReport operator()();

    [[nodiscard]] const std::vector<std::string>& members() const noexcept { return members_; }

private:
    QuoteSource* source_;
    ReferencePublisher* sink_;
    Params params_;
    Universe universe_;
    WallClock wall_;
    std::uint64_t rerank_ns_;
    std::uint64_t ranked_at_{0};
    std::vector<std::string> members_;
    std::map<std::string, AssetState, std::less<>> states_;
};

} // namespace raikiri::reference
