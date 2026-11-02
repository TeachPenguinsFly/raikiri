#pragma once

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/fixed_string.hpp>
#include <raikiri/common/time.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace raikiri {

// twenty fits long tickers such as bingx's NCCOCOFFEE2USD at no size cost
using AssetCode = FixedString<20>;

// folded to upper because asset codes compare across venues; nullopt when empty or too long
[[nodiscard]] std::optional<AssetCode> to_asset_code(std::string_view text) noexcept;

// which of a venue's wallets a balance sits in; unified backs several markets at once, so it is
// counted once per venue however many gateways report it
enum class WalletKind : std::uint8_t { Unknown = 0, Spot, Futures, Unified, Options, Funding };

[[nodiscard]] constexpr std::string_view to_string(WalletKind kind) noexcept {
    switch (kind) {
    case WalletKind::Spot:
        return "spot";
    case WalletKind::Futures:
        return "futures";
    case WalletKind::Unified:
        return "unified";
    case WalletKind::Options:
        return "options";
    case WalletKind::Funding:
        return "funding";
    case WalletKind::Unknown:
        break;
    }
    return "unknown";
}

struct BalanceReport {
    AssetCode asset{};
    // rides the padding after asset; a writer that predates it leaves unknown
    WalletKind wallet{WalletKind::Unknown};

    // undef means not reported, which differs from zero
    FixedE9 total{FixedE9::undef()};

    // undef where the venue publishes only a total
    FixedE9 available{FixedE9::undef()};

    // the venue's own number, since it liquidates on it
    FixedE9 unrealized{FixedE9::undef()};

    UnixNanos ts_venue{};

    [[nodiscard]] bool has_asset() const noexcept { return !asset.empty(); }
};

static_assert(sizeof(BalanceReport) == 56, "BalanceReport rides a shared-memory snapshot");
static_assert(offsetof(BalanceReport, total) == 24);

} // namespace raikiri
