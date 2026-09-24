#pragma once

// bingx gzips every frame under a binary opcode and libs/net does not inflate
// one subscribe message per stream; any string id is echoed

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/inflater.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace raikiri::venues::bingx {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// separate swap and spot hosts; no testnet
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// the perp user stream needs no subscribe; spot needs one
[[nodiscard]] std::string_view private_stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view private_stream_path(MarketType market) noexcept;

// the key rides the url, so a new key needs a new connection
[[nodiscard]] std::string private_stream_target(MarketType market, std::string_view key);

// the 24-hour window on both trees
inline constexpr std::string_view k_ticker_channel = "ticker";

// empty for kinds bingx does not stream, including funding
[[nodiscard]] std::string_view data_type_suffix(exec::SubKind kind, MarketType market) noexcept;

class Protocol {
public:
    struct Frame {
        std::string_view text;
    };

    // {"id":"1","reqType":"sub","dataType":"BTC-USDT@bookTicker"}
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::string_view symbol,
                                                                     std::string_view suffix);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::string_view symbol,
                                                                       std::string_view suffix);

    // the spot user stream subscribes by dataType without a symbol
    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_channel(std::string_view data_type);

    [[nodiscard]] static std::string_view pong_frame() noexcept { return "Pong"; }

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    request(std::string_view req_type, std::string_view symbol, std::string_view suffix);

    std::array<char, 256> frame_{};
    std::uint64_t next_id_{0};
};

} // namespace raikiri::venues::bingx
