#pragma once

// an unknown type closes the socket and an unknown coin gets no reply,
// so subscriptions are validated first

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/exec/data_client.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri::venues::hyperliquid {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

// Returns empty for a kind this venue does not stream under these names.
[[nodiscard]] std::string_view channel_for(exec::SubKind kind) noexcept;

// empty for perp funding and mark, which the metaAndAssetCtxs poll serves
// so the ip's 1000 subscriptions are left to the quote, the book and the tape
[[nodiscard]] std::string_view stream_channel(exec::SubKind kind, MarketType market) noexcept;

// the hip-3 dex named before the colon of a coin, empty for the main dex
[[nodiscard]] std::string_view dex_of(std::string_view coin) noexcept;

struct Subscription {
    std::string_view channel;
    std::string_view coin;
};

class Protocol {
public:
    // {"method":"subscribe","subscription":{"type":"bbo","coin":"BTC"}}
    [[nodiscard]] std::expected<std::string_view, WsBuildError> subscribe_frame(const Subscription& subscription);

    [[nodiscard]] std::expected<std::string_view, WsBuildError> unsubscribe_frame(const Subscription& subscription);

    [[nodiscard]] static std::string_view ping_frame() noexcept { return R"({"method":"ping"})"; }

private:
    [[nodiscard]] std::expected<std::string_view, WsBuildError> method_frame(std::string_view method,
                                                                             const Subscription& subscription);

    std::array<char, 256> frame_{};
};

} // namespace raikiri::venues::hyperliquid
