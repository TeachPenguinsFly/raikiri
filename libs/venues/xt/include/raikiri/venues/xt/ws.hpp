#pragma once

// bbo comes from the depth snapshot; agg_ticker has no sizes and bbo@/book_ticker@ do not exist

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// xt has no futures testnet; the spot demo host does not serve fstream
[[nodiscard]] std::string_view stream_host(MarketType market) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path(MarketType market) noexcept;

// depth params: levels must be 5/10/20/50 and interval 100ms/250ms/1000ms
struct Topic {
    std::string_view channel; // empty for a kind XT does not stream
    std::string_view params;
};

[[nodiscard]] Topic topic_for(exec::SubKind kind, MarketType market) noexcept;

// rides with the bbo subscription since xt streams no funding; spot needs it too
[[nodiscard]] Topic ticker_topic() noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view symbol;
    std::string_view params;
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    // an application text ping; xt closes a socket that sends none for 30 s, public included
    [[nodiscard]] static std::string_view ping_frame() noexcept;

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> method_frame(std::string_view method,
                                                                  std::span<const Subscription> subscriptions);

    std::array<char, 4096> frame_{};
    std::uint64_t next_id_{1};
};

// futures: fstream /ws/user, "SUBSCRIBE", listen key appended to each topic with @;
// spot: stream /private, "subscribe", token in a top-level listenKey field

[[nodiscard]] std::string_view private_stream_host() noexcept;
[[nodiscard]] std::string_view private_stream_path() noexcept;
[[nodiscard]] std::string_view spot_private_stream_host() noexcept;
[[nodiscard]] std::string_view spot_private_stream_path() noexcept;

// code 2 means the key expired: mint a new one and resubscribe
inline constexpr std::int64_t k_listen_key_invalid = 2;

class PrivateProtocol {
public:
    // order and trade only; GatewaySink has no channel for position, balance or notify
    [[nodiscard]] std::expected<std::string_view, WsBuildError> futures_subscribe(std::string_view listen_key);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> spot_subscribe(std::string_view access_token);

    // an application text ping; xt closes a socket that sends none for 30 s
    [[nodiscard]] static std::string_view ping_frame() noexcept;

private:
    // the spot token is a jwt of several hundred bytes
    std::array<char, 2048> frame_{};
    std::uint64_t next_id_{1};
};

} // namespace raikiri::venues::xt
