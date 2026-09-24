#pragma once

// subscribe fails whole on a refused channel; unknown channels are silently dropped from the reply

#include <raikiri/exec/data_client.hpp>
#include <raikiri/net/signer.hpp>
#include <raikiri/venues/deribit/endpoints.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::deribit {

enum class WsBuildError : std::uint8_t {
    UnsupportedKind,
    NothingToSend,
    BufferTooSmall,
};

[[nodiscard]] std::string_view to_string(WsBuildError err) noexcept;

// testnet is a separate exchange with its own ids
[[nodiscard]] std::string_view stream_host(bool testnet) noexcept;
[[nodiscard]] std::string_view stream_port() noexcept;
[[nodiscard]] std::string_view stream_path() noexcept;

inline constexpr std::size_t k_max_channel_len = 64;

// per kind and currency so fills on unwatched instruments are seen; raw needs an authenticated socket
[[nodiscard]] std::string_view user_changes_channel(std::string_view kind,
                                                    std::string_view currency,
                                                    std::span<char> into) noexcept;

[[nodiscard]] std::string_view
channel_for(exec::SubKind kind, std::string_view instrument, std::span<char> into) noexcept;

struct Subscription {
    std::string_view channel;
};


class Protocol {
public:
    struct Frame {
        std::string_view text;
        std::size_t consumed{0};
        std::int64_t id{0}; // correlates the reply that reports what was accepted
    };

    [[nodiscard]] std::expected<Frame, WsBuildError>
    subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError>
    unsubscribe_frame(std::span<const Subscription> subscriptions);

    // legal only after this socket authenticated
    [[nodiscard]] std::expected<Frame, WsBuildError>
    private_subscribe_frame(std::span<const Subscription> subscriptions);

    [[nodiscard]] std::expected<Frame, WsBuildError> auth_frame(const net::Signer& signer,
                                                                UnixNanos now);

    // seconds; deribit's floor is 10
    [[nodiscard]] Frame set_heartbeat_frame(std::uint32_t interval_s);

    [[nodiscard]] Frame test_frame();

private:
    [[nodiscard]] std::expected<Frame, WsBuildError>
    channel_frame(std::string_view scope,
                  std::string_view method,
                  std::span<const Subscription> subscriptions);

    std::array<char, ep::k_max_frame_bytes> frame_{};
    std::array<char, 24> nonce_{};
    std::array<char, 192> sign_msg_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::int64_t next_id_{1};
    std::uint64_t nonce_counter_{0};
};

} // namespace raikiri::venues::deribit
