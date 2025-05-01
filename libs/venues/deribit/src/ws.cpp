#include <raikiri/venues/deribit/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::deribit {

namespace {

constexpr std::string_view k_host_live = "www.deribit.com";
constexpr std::string_view k_host_test = "test.deribit.com";

// The only cadence open to an unauthenticated socket: "raw" is refused with
// 13778 (verified live).
constexpr std::string_view k_interval = "100ms";

// Drops anything that would overflow rather than truncating mid-token: a
// half-written channel names a different instrument.
class TextWriter {
public:
    explicit TextWriter(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > into_.size()) {
            overflow_ = true;
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    void put(std::int64_t value) noexcept {
        std::array<char, 24> num{};
        if (const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
            ec == std::errc{}) {
            put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
        }
    }

    [[nodiscard]] bool overflow() const noexcept { return overflow_; }

    [[nodiscard]] std::string_view view() const noexcept { return {into_.data(), len_}; }

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
    return testnet ? k_host_test : k_host_live;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/ws/api/v2";
}

std::string_view
channel_for(exec::SubKind kind, std::string_view instrument, std::span<char> into) noexcept {
    if (instrument.empty()) {
        return {};
    }
    TextWriter writer{into};
    switch (kind) {
    case exec::SubKind::Bbo:
        writer.put("book.");
        writer.put(instrument);
        writer.put(".none.1.");
        writer.put(k_interval);
        break;
    case exec::SubKind::Trades:
        writer.put("trades.");
        writer.put(instrument);
        writer.put(".");
        writer.put(k_interval);
        break;
    case exec::SubKind::Depth:
        writer.put("book.");
        writer.put(instrument);
        writer.put(".");
        writer.put(k_interval);
        break;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        writer.put("ticker.");
        writer.put(instrument);
        writer.put(".");
        writer.put(k_interval);
        break;
    case exec::SubKind::Kline:
        return {}; // chart.trades needs a bar size; no consumer yet
    }
    return writer.overflow() ? std::string_view{} : writer.view();
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::channel_frame(std::string_view method, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = std::min(subscriptions.size(), k_max_subscriptions_per_frame);
    for (std::size_t i = 0; i < batch; ++i) {
        if (subscriptions[i].channel.empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
    }

    const auto id = next_id_++;
    TextWriter writer{frame_};
    writer.put(R"({"jsonrpc":"2.0","id":)");
    writer.put(id);
    writer.put(R"(,"method":"public/)");
    writer.put(method);
    writer.put(R"(","params":{"channels":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            writer.put(",");
        }
        writer.put("\"");
        writer.put(subscriptions[i].channel);
        writer.put("\"");
    }
    writer.put("]}}");
    if (writer.overflow()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = writer.view(), .consumed = batch, .id = id};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return channel_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return channel_frame("unsubscribe", subscriptions);
}

Protocol::Frame Protocol::set_heartbeat_frame(std::uint32_t interval_s) {
    const auto id = next_id_++;
    TextWriter writer{frame_};
    writer.put(R"({"jsonrpc":"2.0","id":)");
    writer.put(id);
    writer.put(R"(,"method":"public/set_heartbeat","params":{"interval":)");
    writer.put(static_cast<std::int64_t>(interval_s));
    writer.put("}}");
    return {.text = writer.view(), .consumed = 0, .id = id};
}

Protocol::Frame Protocol::test_frame() {
    const auto id = next_id_++;
    TextWriter writer{frame_};
    writer.put(R"({"jsonrpc":"2.0","id":)");
    writer.put(id);
    writer.put(R"(,"method":"public/test","params":{}})");
    return {.text = writer.view(), .consumed = 0, .id = id};
}

} // namespace raikiri::venues::deribit
