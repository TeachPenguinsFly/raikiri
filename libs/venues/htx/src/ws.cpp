#include <raikiri/venues/htx/ws.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::htx {

namespace {

constexpr std::string_view k_host = "api.hbdm.com";
// Fills the protocol's frame buffer. A part that would overflow marks the
// whole frame bad rather than truncating it: a half-written subscribe names a
// topic HTX refuses.
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
        std::array<char, 24> digits{};
        const auto [stop, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return;
        }
        put(std::string_view{digits.data(), static_cast<std::size_t>(stop - digits.data())});
    }

    [[nodiscard]] std::expected<Protocol::Frame, WsBuildError> frame() const noexcept {
        if (overflow_) {
            return std::unexpected{WsBuildError::BufferTooSmall};
        }
        return Protocol::Frame{.text = {into_.data(), len_}};
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
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host() noexcept {
    return k_host;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view market_path() noexcept {
    return "/linear-swap-ws";
}

std::string_view notify_path() noexcept {
    return "/linear-swap-notification";
}

std::string_view topic_suffix(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bbo";
    case exec::SubKind::Trades:
        return "trade.detail";
    case exec::SubKind::Depth:
        return "depth.step0";
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::request(std::string_view op, std::string_view code, std::string_view suffix) {
    if (code.empty() || suffix.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(R"({")");
    writer.put(op);
    writer.put(R"(":"market.)");
    writer.put(code);
    writer.put(".");
    writer.put(suffix);
    writer.put(R"(","id":")");
    writer.put(++next_id_);
    writer.put(R"("})");
    return writer.frame();
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::string_view code,
                                                                       std::string_view suffix) {
    return request("sub", code, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::string_view code,
                                                                         std::string_view suffix) {
    return request("unsub", code, suffix);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::funding_subscribe_frame(std::string_view code) {
    if (code.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(R"({"op":"sub","topic":"public.)");
    writer.put(code);
    writer.put(R"(.funding_rate","cid":")");
    writer.put(++next_id_);
    writer.put(R"("})");
    return writer.frame();
}

// The timestamp goes back as the token it arrived as, quotes and all.
std::expected<Protocol::Frame, WsBuildError> Protocol::pong(std::string_view head,
                                                            std::string_view ts) {
    if (ts.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(head);
    writer.put(ts);
    writer.put("}");
    return writer.frame();
}

std::expected<Protocol::Frame, WsBuildError> Protocol::market_pong_frame(std::string_view ts) {
    return pong(R"({"pong":)", ts);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::notify_pong_frame(std::string_view ts) {
    return pong(R"({"op":"pong","ts":)", ts);
}

} // namespace raikiri::venues::htx
