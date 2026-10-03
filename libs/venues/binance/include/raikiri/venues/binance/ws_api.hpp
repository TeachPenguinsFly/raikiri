#pragma once

// the usd-m websocket api (ws-fapi) carries the rest builder's query, re-signed with its params sorted
// session.logon takes only ed25519 keys and ours are hmac, so every request is signed

#include <raikiri/net/signer.hpp>
#include <raikiri/net/ws_rpc.hpp>
#include <raikiri/venues/binance/envelope.hpp>
#include <raikiri/venues/binance/rest.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace raikiri::venues::binance {

inline constexpr std::string_view k_ws_order_place = "order.place";
inline constexpr std::string_view k_ws_order_cancel = "order.cancel";
inline constexpr std::string_view k_ws_order_modify = "order.modify";

struct WsApiReply {
    std::uint64_t id{0}; // zero when the frame carries no numeric id
    std::uint32_t status{0};
    RestError error{};
};

[[nodiscard]] std::expected<WsApiReply, WireError> parse_ws_api_reply(std::string_view frame);

class WsApiBuilder {
public:
    explicit WsApiBuilder(const net::Signer& signer) noexcept : signer_{&signer} {}

    // `query` is what the rest builder signed for the same command, before its signature
    [[nodiscard]] std::expected<std::string_view, BuildError>
    request(std::uint64_t id, std::string_view method, std::string_view query);

    // exposed for tests: the exact bytes the last request signed
    [[nodiscard]] std::string_view last_sign_message() const noexcept { return {sign_.data(), sign_len_}; }

private:
    const net::Signer* signer_;
    std::array<char, 768> sign_{};
    std::array<char, 1024> frame_{};
    std::array<char, 512> values_{};
    std::array<char, net::k_hex_bytes> signature_{};
    std::size_t sign_len_{0};
};

class WsApiCodec final : public net::WsRpcCodec {
public:
    explicit WsApiCodec(const net::Signer& signer) noexcept : signer_{&signer} {}

    [[nodiscard]] std::optional<std::string> login(UnixNanos now) override;
    [[nodiscard]] net::WsRpcFrame classify(std::string_view payload) noexcept override;

private:
    const net::Signer* signer_;
};

} // namespace raikiri::venues::binance
