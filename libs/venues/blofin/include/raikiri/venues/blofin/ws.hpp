#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::blofin {

// `books` is also the quote: there is no bbo channel
inline constexpr std::string_view k_books_channel = "books";
inline constexpr std::string_view k_trades_channel = "trades";
inline constexpr std::string_view k_funding_channel = "funding-rate";
inline constexpr std::string_view k_tickers_channel = "tickers";

// the venue closes a socket silent for 30 s; its docs ask for a text ping
inline constexpr std::uint64_t k_ping_interval_ns = 15ULL * 1'000'000'000ULL;

enum class WsBuildError : std::uint8_t {
    NothingToSend,
    BufferTooSmall,
};

class Protocol {
public:
    // one instrument per channel, so a refused frame costs only that one
    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    subscribe_frame(std::string_view inst_id, std::span<const std::string_view> channels);

    [[nodiscard]] std::expected<std::string_view, WsBuildError>
    unsubscribe_frame(std::string_view inst_id, std::span<const std::string_view> channels);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return "ping"; }

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> op_frame(
        std::string_view op, std::string_view inst_id, std::span<const std::string_view> channels);

    std::array<char, 1024> frame_{};
};

} // namespace raikiri::venues::blofin
