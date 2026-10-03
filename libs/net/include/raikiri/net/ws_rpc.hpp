#pragma once

// one persistent logged-in socket carrying request/reply commands, matched by request id
// a request unanswered by its deadline, or in flight when the socket drops, ends Unknown and is never resent

#include <raikiri/common/time.hpp>
#include <raikiri/net/ws_client.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raikiri::net {

class IoContext;

// what the channel needs from one inbound frame
struct WsRpcFrame {
    enum class Kind : std::uint8_t { Reply, Login, Challenge, Pong, Other };

    Kind kind{Kind::Other};
    std::uint64_t id{0};
    bool ok{false};
    std::string_view detail{};
};

// a venue's envelope: how to log in, ping, and find the request id in a reply
class WsRpcCodec {
public:
    WsRpcCodec() = default;
    WsRpcCodec(const WsRpcCodec&) = delete;
    WsRpcCodec& operator=(const WsRpcCodec&) = delete;

    // nullopt cannot log in (no credentials); an empty frame means the socket is usable once open
    [[nodiscard]] virtual std::optional<std::string> login(UnixNanos now) = 0;

    [[nodiscard]] virtual WsRpcFrame classify(std::string_view payload) noexcept = 0;

    // empty when protocol pings keep the socket alive
    [[nodiscard]] virtual std::string_view ping() const noexcept { return {}; }

    // a venue that opens with a challenge to sign: login() is then empty and answer() replies to it
    [[nodiscard]] virtual bool challenged() const noexcept { return false; }

    [[nodiscard]] virtual std::optional<std::string> answer(std::string_view) { return std::nullopt; }

protected:
    ~WsRpcCodec() = default;
};

enum class WsRpcState : std::uint8_t { Down, LoggingIn, Ready, AuthFailed };

[[nodiscard]] std::string_view to_string(WsRpcState state) noexcept;

enum class WsRpcOutcome : std::uint8_t {
    Answered,
    Unknown, // no reply by the deadline, or the socket dropped with it in flight
};

struct WsRpcReply {
    WsRpcOutcome outcome{WsRpcOutcome::Unknown};
    std::string_view payload{}; // the venue's frame, valid during the callback
    std::string_view detail{};
};

using WsRpcCallback = std::function<void(const WsRpcReply& reply)>;

struct WsRpcConfig {
    WsConfig ws{};
    std::uint64_t request_timeout_ns{10'000'000'000ULL};
    std::uint64_t login_timeout_ns{10'000'000'000ULL};
    // a refused key is not fixed by retrying at once
    std::uint64_t login_retry_ns{30'000'000'000ULL};
    std::uint64_t ping_interval_ns{0};
    std::size_t max_in_flight{64};
};

struct WsRpcStats {
    std::uint64_t sent{0};
    std::uint64_t answered{0};
    std::uint64_t unknown{0};
    std::uint64_t unmatched{0};
    std::uint64_t logins{0};
    std::uint64_t login_failures{0};
    std::uint64_t login_rtt_ns{0};
    std::uint64_t ping_rtt_ns{0};
};

class WsRpc final : public WsHandler {
public:
    using Link = std::function<bool(std::string_view frame)>;
    using Observer = std::function<void(WsRpcState state, std::string_view detail)>;
    // asked before each request; false keeps it off the socket so the caller can route it elsewhere
    using Admit = std::function<bool()>;

    WsRpc(IoContext& io, const Clock& clock, WsRpcConfig config, WsRpcCodec& codec);

    // frames go to `link` and the caller drives the WsHandler calls; for tests
    WsRpc(const Clock& clock, WsRpcConfig config, WsRpcCodec& codec, Link link);

    ~WsRpc();
    WsRpc(const WsRpc&) = delete;
    WsRpc& operator=(const WsRpc&) = delete;
    WsRpc(WsRpc&&) = delete;
    WsRpc& operator=(WsRpc&&) = delete;

    void start();
    // every request in flight ends Unknown
    void stop();

    void set_observer(Observer observer);
    void set_admit(Admit admit);

    [[nodiscard]] WsRpcState state() const noexcept { return state_; }

    [[nodiscard]] bool ready() const noexcept { return state_ == WsRpcState::Ready; }

    [[nodiscard]] std::uint64_t next_id() noexcept { return ++last_id_; }

    // false when not ready, full, not admitted or the write was refused; nothing then left this process
    [[nodiscard]] bool request(std::uint64_t id, std::string_view frame, WsRpcCallback on_reply);

    // deadlines, the login timeout, the venue's ping and the retry after a refused login
    void poll() noexcept;

    [[nodiscard]] std::size_t in_flight() const noexcept { return pending_.size(); }

    [[nodiscard]] const WsRpcStats& stats() const noexcept { return stats_; }

    void on_open() override;
    void on_message(std::string_view payload, UnixNanos received) noexcept override;
    void on_close(WsCloseReason reason) override;

private:
    struct Pending {
        std::uint64_t id{0};
        std::uint64_t deadline_ns{0};
        WsRpcCallback on_reply;
    };

    [[nodiscard]] bool write(std::string_view frame);
    void enter(WsRpcState state, std::string_view detail);
    void fail_all(std::string_view detail);
    void restart();

    const Clock* clock_;
    WsRpcConfig config_;
    WsRpcCodec* codec_;
    Link link_;
    std::unique_ptr<WsClient> ws_;
    Observer observer_;
    Admit admit_;

    WsRpcState state_{WsRpcState::Down};
    bool running_{false};
    std::uint64_t last_id_{0};
    std::uint64_t login_sent_ns_{0};
    std::uint64_t retry_at_ns_{0};
    std::uint64_t next_ping_ns_{0};
    std::uint64_t ping_sent_ns_{0};
    std::vector<Pending> pending_;
    WsRpcStats stats_{};
};

} // namespace raikiri::net
