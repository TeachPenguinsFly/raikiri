#pragma once

// the rows of the ref.mid segment; a field change is an abi change of k_ref_tag

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/reference/engine.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace raikiri::reference {

enum RefFlag : std::uint32_t {
    k_flag_refused = 1U << 0,
    k_flag_usdt_fallback = 1U << 1,
    k_flag_usdc_fallback = 1U << 2,
};

struct RefCell {
    char asset[24]{};
    FixedE9 mid{};
    FixedE9 spot_mid{};
    FixedE9 perp_mid{};
    FixedE9 basis_e9{};
    FixedE9 conf{};
    FixedE9 dispersion_e9{};
    FixedE9 usdt_usd{};
    FixedE9 usdc_usd{};
    // wall clock of the cycle that wrote it
    std::uint64_t ts_ns{0};
    std::uint64_t oldest_age_ns{0};
    std::uint16_t quotes{0};
    std::uint16_t used_spot{0};
    std::uint16_t used_perp{0};
    std::uint16_t pad_{0};
    std::array<std::uint16_t, k_cut_count> cuts{};
    std::uint32_t flags{0};
    std::uint32_t pad2_{0};

    [[nodiscard]] std::string_view asset_key() const noexcept;
};

static_assert(sizeof(RefCell) == 136);
static_assert(std::is_trivially_copyable_v<RefCell>);

struct RefMetrics {
    std::uint64_t published_ns{0};
    std::uint64_t period_ns{0};
    std::uint64_t took_ns{0};
    std::uint64_t cycles{0};
    std::uint32_t gateways{0};
    // gateways read without a reader slot
    std::uint32_t unlisted{0};
    std::uint32_t quotes{0};
    std::uint32_t assets{0};
    std::uint32_t refused{0};
    std::uint32_t flags{0};
    FixedE9 usdt_usd{};
    FixedE9 usdc_usd{};
    std::uint32_t usdt_sources{0};
    std::uint32_t usdc_sources{0};
};

static_assert(std::is_trivially_copyable_v<RefMetrics>);

[[nodiscard]] RefCell to_cell(const Row& row, std::uint64_t ts_ns) noexcept;

// what a reader of the segment gets in one pass
struct RefSnapshot {
    bool owner_alive{false};
    RefMetrics metrics{};
    std::vector<RefCell> rows{};
};

class ReferenceFeed {
public:
    ReferenceFeed() = default;
    ReferenceFeed(const ReferenceFeed&) = delete;
    ReferenceFeed& operator=(const ReferenceFeed&) = delete;
    virtual ~ReferenceFeed() = default;

    // empty while no refd publishes
    [[nodiscard]] virtual std::optional<RefSnapshot> read() = 0;
};

} // namespace raikiri::reference
