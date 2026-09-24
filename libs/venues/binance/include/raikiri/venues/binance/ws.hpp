#pragma once

#include <raikiri/exec/data_client.hpp>
#include <raikiri/exec/instrument_table.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    TooManyStreams,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;

// the multiplexed endpoint that delivers kind; frames arrive wrapped
[[nodiscard]] std::string_view stream_path(exec::SubKind kind) noexcept;

// 500 ms: at 100 ms the venue drops sockets whose reader falls behind
inline constexpr std::string_view k_depth_stream = "@depth@500ms";
inline constexpr std::string_view k_mark_price_stream = "@markPrice@1s";
inline constexpr std::string_view k_bbo_stream = "@bookTicker";
inline constexpr std::string_view k_trade_stream = "@trade";

// lowercases the symbol, which binance requires; empty for a kind binance does not stream
[[nodiscard]] std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const std::string_view> streams);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const std::string_view> streams);

    [[nodiscard]] std::uint64_t last_id() const noexcept { return next_id_ - 1; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    method_frame(std::string_view method, std::span<const std::string_view> streams);

    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::binance
