#pragma once

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::venues::lighter {

enum class WsBuildError : std::uint8_t {
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// The book is the quote: ticker/<id> is not asked for, which saves a third of the budget.
inline constexpr std::string_view k_book_channel = "order_book";
inline constexpr std::string_view k_trade_channel = "trade";

// The only `all` channels that cover every market; ticker/all is acknowledged but
// serves market 0 alone.
inline constexpr std::string_view k_perp_stats_all = "market_stats/all";
inline constexpr std::string_view k_spot_stats_all = "spot_market_stats/all";

// A reopen resends the whole set at that pace, so books go first, then the stats, then
// trades; each channel once.
[[nodiscard]] std::vector<std::string_view> ask_order(std::span<const std::string> channels);

// Empty for a kind not read per market.
[[nodiscard]] std::string_view channel_name(exec::SubKind kind, std::uint32_t market_id, std::span<char> out) noexcept;

// The channel a subscribe frame of ours asks for; empty for any other frame.
[[nodiscard]] std::string_view subscribed_channel(std::string_view frame) noexcept;

class Protocol {
public:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> subscribe_frame(std::string_view channel);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> unsubscribe_frame(std::string_view channel);

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> frame(std::string_view type, std::string_view channel);

    std::array<char, 128> frame_{};
};

} // namespace raikiri::venues::lighter
