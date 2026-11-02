#pragma once

#include <raikiri/clientapi/application/hub.hpp>
#include <raikiri/clientapi/application/refusal.hpp>
#include <raikiri/common/time.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::clientapi {

[[nodiscard]] std::string problem_body(const Refusal& refusal, std::string_view request_id);

[[nodiscard]] std::string private_frame(const PrivateEvent& event);
[[nodiscard]] std::string
hello_frame(std::string_view epoch, std::uint64_t seq, std::uint32_t heartbeat_ms, std::string_view client_id);
[[nodiscard]] std::string heartbeat_frame(UnixNanos ts, std::uint64_t seq);
[[nodiscard]] std::string ack_frame(std::string_view id, std::uint64_t seq);
[[nodiscard]] std::string pong_frame(std::string_view id);
[[nodiscard]] std::string gap_frame(std::uint64_t from, std::uint64_t to, std::string_view reason);
[[nodiscard]] std::string stream_problem_frame(std::string_view id, const Refusal& refusal);

struct ClientOp {
    std::string op{};
    std::string id{};
    std::uint8_t channels{0};
    std::optional<std::uint64_t> resume_from{};
    std::string epoch{};
    // every instrument a market subscribe names, for the scope check
    std::vector<std::string> instruments{};
    // the same subscribe, rebuilt from known fields only, for the backend
    std::string market_message{};
};

[[nodiscard]] std::expected<ClientOp, Refusal> parse_client_op(std::string_view text);
[[nodiscard]] std::string market_unsubscribe(std::string_view id);
[[nodiscard]] std::string private_link_subscribe();

struct UpstreamFrame {
    std::optional<OrderEventRow> order_event{};
    std::optional<FillRow> fill{};
    std::optional<std::string> gap{};
};

// nullopt for a frame that carries no event
[[nodiscard]] std::optional<UpstreamFrame> parse_upstream_frame(std::string_view text);

} // namespace raikiri::clientapi
