#pragma once

// each channel carries its own product_ids, so one frame can mix products per channel

#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::coinbase {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// coinbase retired the exchange sandbox; there is no testnet host
[[nodiscard]] std::string_view stream_host() noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// empty for a kind coinbase does not stream
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view product_id;
};

class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
    };

    [[nodiscard]] std::expected<Frame, WsBuildError> subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError> unsubscribe_frame(std::span<const Subscription> subscriptions);

private:
    [[nodiscard]] std::expected<Frame, WsBuildError> op_frame(std::string_view type,
                                                              std::span<const Subscription> subscriptions);

    std::array<char, 8192> frame_{};
};

} // namespace raikiri::venues::coinbase
