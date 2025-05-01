#include <raikiri/venues/gate/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::gate {

namespace {

constexpr std::string_view k_host_live = "fx-ws.gateio.ws";
constexpr std::string_view k_host_testnet = "fx-ws-testnet.gateio.ws";

constexpr std::string_view k_channel_depth = "futures.order_book_update";

// Writes into a caller-owned buffer, refusing rather than truncating: a
// half-written frame would subscribe to something other than what was asked.
class FrameWriter {
public:
    explicit FrameWriter(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > into_.size()) {
            overflow_ = true;
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    void put(std::uint64_t value) noexcept {
        std::array<char, 24> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] bool overflow() const noexcept { return overflow_; }

    [[nodiscard]] std::string_view text() const noexcept {
        return std::string_view{into_.data(), len_};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::NothingToSend:
        return "NothingToSend";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(bool testnet) noexcept {
    return testnet ? k_host_testnet : k_host_live;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/v4/ws/usdt";
}

std::string_view channel_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "futures.book_ticker";
    case exec::SubKind::Trades:
        return "futures.trades";
    case exec::SubKind::Depth:
        return k_channel_depth;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "futures.tickers";
    case exec::SubKind::Kline:
        return {}; // needs an interval in the payload; no consumer yet
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::op_frame(
    std::string_view op, std::span<const Subscription> subscriptions, std::uint64_t now_s) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto channel = subscriptions.front().channel;
    if (channel.empty() || subscriptions.front().contract.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t batch = 1;
    if (channel != k_channel_depth) {
        const auto limit = std::min(subscriptions.size(), k_max_subscriptions_per_frame);
        while (batch < limit && subscriptions[batch].channel == channel &&
               !subscriptions[batch].contract.empty()) {
            ++batch;
        }
    }

    FrameWriter writer{frame_};
    writer.put(R"({"time":)");
    writer.put(now_s);
    writer.put(R"(,"channel":")");
    writer.put(channel);
    writer.put(R"(","event":")");
    writer.put(op);
    writer.put(R"(","payload":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            writer.put(",");
        }
        writer.put("\"");
        writer.put(subscriptions[i].contract);
        writer.put("\"");
    }
    if (channel == k_channel_depth) {
        writer.put(",\"");
        writer.put(k_depth_interval);
        writer.put("\",\"");
        writer.put(k_depth_level);
        writer.put("\"");
    }
    writer.put("]}");
    if (writer.overflow()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = writer.text(), .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions, std::uint64_t now_s) {
    return op_frame("subscribe", subscriptions, now_s);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions, std::uint64_t now_s) {
    return op_frame("unsubscribe", subscriptions, now_s);
}

std::string_view Protocol::ping_frame(std::uint64_t now_s) noexcept {
    FrameWriter writer{ping_};
    writer.put(R"({"time":)");
    writer.put(now_s);
    writer.put(R"(,"channel":"futures.ping"})");
    return writer.overflow() ? std::string_view{} : writer.text();
}

} // namespace raikiri::venues::gate
