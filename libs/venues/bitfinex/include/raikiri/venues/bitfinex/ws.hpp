#pragma once

// one channel per subscribe; unsubscribe is by chanId
// app-level ping covers sockets with no subscriptions, which get no heartbeats

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::bitfinex {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

enum class ChannelKind : std::uint8_t {
    Ticker,
    Trades,
    Book,
    DerivStatus,
    Unknown,
};

// the status channel carries several feeds; the key decides
[[nodiscard]] ChannelKind channel_kind(std::string_view channel, std::string_view key) noexcept;

[[nodiscard]] std::string_view channel_name(ChannelKind kind) noexcept;

// Unknown for kinds not streamed publicly; ticker is a slow sample and the book supplies the fast bbo
[[nodiscard]] ChannelKind kind_for(exec::SubKind kind) noexcept;

// the status ack has no symbol, so this is the route back to an instrument
[[nodiscard]] std::string_view symbol_from_deriv_key(std::string_view key) noexcept;

struct Subscription {
    ChannelKind kind{ChannelKind::Unknown};
    std::string_view symbol; // "tBTCUSD"
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(const Subscription& sub);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::int64_t chan_id);

    [[nodiscard]] static std::string_view conf_frame() noexcept;

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"event":"ping"})"; }

private:
    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::bitfinex
