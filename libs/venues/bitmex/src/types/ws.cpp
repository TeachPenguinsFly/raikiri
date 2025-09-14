#include <raikiri/venues/bitmex/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::bitmex {

namespace {

constexpr std::string_view k_host_live = "ws.bitmex.com";
constexpr std::string_view k_host_test = "ws.testnet.bitmex.com";

// Appends into the protocol's frame buffer, reporting overflow rather than
// truncating: half an auth frame is still valid JSON often enough to be
// accepted as something other than a login.
class Writer {
public:
    explicit Writer(std::span<char> into) noexcept : into_{into} {}

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
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        put(std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())});
    }

    [[nodiscard]] std::expected<Protocol::Frame, WsBuildError>
    frame(std::size_t consumed) const noexcept {
        if (overflow_) {
            return std::unexpected{WsBuildError::BufferTooSmall};
        }
        return Protocol::Frame{.text = {into_.data(), len_}, .consumed = consumed};
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
    return testnet ? k_host_test : k_host_live;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return "/realtime";
}

std::string_view topic_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "quote";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        return "orderBookL2_25";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "instrument";
    case exec::SubKind::Kline:
        return {}; // tradeBinNm — needs a bar size in the name; no consumer yet
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::op_frame(std::string_view op, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto batch = std::min(subscriptions.size(), k_max_subscriptions_per_frame);
    for (std::size_t i = 0; i < batch; ++i) {
        if (subscriptions[i].table.empty() || subscriptions[i].symbol.empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
    }

    std::size_t len = 0;
    bool overflow = false;
    const auto put = [&](std::string_view text) {
        if (overflow || len + text.size() > frame_.size()) {
            overflow = true;
            return;
        }
        std::memcpy(frame_.data() + len, text.data(), text.size());
        len += text.size();
    };
    put(R"({"op":")");
    put(op);
    put(R"(","args":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(subscriptions[i].table);
        put(":");
        put(subscriptions[i].symbol);
        put("\"");
    }
    put("]}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return op_frame("unsubscribe", subscriptions);
}

std::string_view auth_message(std::uint64_t expires, std::span<char> out) noexcept {
    Writer writer{out};
    // Bare concatenation of the socket's own verb and path with the expiry —
    // the same shape a REST request signs, with no body to follow it.
    writer.put("GET");
    writer.put(stream_path());
    writer.put(expires);
    const auto frame = writer.frame(0);
    return frame.has_value() ? frame->text : std::string_view{};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::auth_frame(std::string_view api_key, std::uint64_t expires, std::string_view signature) {
    if (api_key.empty() || signature.empty() || expires == 0) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    Writer writer{frame_};
    writer.put(R"({"op":"authKeyExpires","args":[")");
    writer.put(api_key);
    writer.put(R"(",)");
    writer.put(expires);
    writer.put(R"(,")");
    writer.put(signature);
    writer.put(R"("]})");
    return writer.frame(0);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::private_subscribe_frame(std::span<const std::string_view> tables) {
    if (tables.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    Writer writer{frame_};
    writer.put(R"({"op":"subscribe","args":[)");
    for (std::size_t i = 0; i < tables.size(); ++i) {
        if (tables[i].empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
        if (i != 0) {
            writer.put(",");
        }
        writer.put("\"");
        writer.put(tables[i]);
        writer.put("\"");
    }
    writer.put("]}");
    return writer.frame(tables.size());
}

} // namespace raikiri::venues::bitmex
