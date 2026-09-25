#include <raikiri/venues/htx/endpoints.hpp>
#include <raikiri/venues/htx/sign.hpp>
#include <raikiri/venues/htx/ws.hpp>

#include <charconv>
#include <cstring>
#include <span>

namespace raikiri::venues::htx {

namespace {

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
    return ep::k_stream_futures;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view market_path() noexcept {
    return ep::k_ws_market;
}

std::string_view notify_path() noexcept {
    return ep::k_ws_notify;
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

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::string_view code, std::string_view suffix) {
    return request("sub", code, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::string_view code,
                                                                         std::string_view suffix) {
    return request("unsub", code, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::funding_subscribe_frame(std::string_view code) {
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

// the timestamp goes back as it arrived, quotes and all
std::expected<Protocol::Frame, WsBuildError> Protocol::pong(std::string_view head, std::string_view ts) {
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

std::expected<Protocol::Frame, WsBuildError> Protocol::spot_pong_frame(std::string_view ts) {
    if (ts.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(R"({"action":"pong","data":{"ts":)");
    writer.put(ts);
    writer.put("}}");
    return writer.frame();
}

std::string_view spot_stream_host() noexcept {
    return ep::k_stream_spot;
}

std::string_view spot_private_path() noexcept {
    return ep::k_ws_spot_private;
}

std::string_view Protocol::last_auth_message() const noexcept {
    return {auth_msg_.data(), auth_msg_len_};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::swap_auth_frame(const net::Signer& signer, UnixNanos now) {
    if (!signer.has_credentials()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    const auto ts = sign::timestamp(now.ns / 1'000'000'000ULL, timestamp_);
    if (ts.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    sign::Query query;
    query.add("AccessKeyId", signer.api_key());
    query.add("SignatureMethod", "HmacSHA256");
    query.add("SignatureVersion", "2");
    query.add("Timestamp", ts);
    const auto rendered = query.render(auth_query_);
    if (rendered.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    const auto message = sign::canonical("GET", ep::k_stream_futures, notify_path(), rendered, auth_msg_);
    if (message.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    auth_msg_len_ = message.size();
    const auto signature = signer.sign(message, net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }

    // the digest travels as a json string, so it is not percent-encoded here
    FrameWriter writer{auth_frame_};
    writer.put(R"({"op":"auth","type":"api","AccessKeyId":")");
    writer.put(signer.api_key());
    writer.put(R"(","SignatureMethod":"HmacSHA256","SignatureVersion":"2","Timestamp":")");
    writer.put(ts);
    writer.put(R"(","Signature":")");
    writer.put(signature);
    writer.put(R"("})");
    return writer.frame();
}

std::expected<Protocol::Frame, WsBuildError> Protocol::spot_auth_frame(const net::Signer& signer, UnixNanos now) {
    if (!signer.has_credentials()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    const auto ts = sign::timestamp(now.ns / 1'000'000'000ULL, timestamp_);
    if (ts.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    sign::Query query;
    query.add("accessKey", signer.api_key());
    query.add("signatureMethod", "HmacSHA256");
    query.add("signatureVersion", "2.1");
    query.add("timestamp", ts);
    const auto rendered = query.render(auth_query_);
    if (rendered.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    const auto message = sign::canonical("GET", ep::k_stream_spot, ep::k_ws_spot_private, rendered, auth_msg_);
    if (message.empty()) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    auth_msg_len_ = message.size();
    const auto signature = signer.sign(message, net::Signer::Encoding::Base64, signature_);
    if (signature.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }

    FrameWriter writer{auth_frame_};
    writer.put(R"({"action":"req","ch":"auth","params":{"authType":"api","accessKey":")");
    writer.put(signer.api_key());
    writer.put(R"(","signatureMethod":"HmacSHA256","signatureVersion":"2.1","timestamp":")");
    writer.put(ts);
    writer.put(R"(","signature":")");
    writer.put(signature);
    writer.put(R"("}})");
    return writer.frame();
}

std::expected<Protocol::Frame, WsBuildError> Protocol::swap_private_subscribe_frame(std::string_view topic) {
    if (topic.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(R"({"op":"sub","topic":")");
    writer.put(topic);
    writer.put(R"(","cid":")");
    writer.put(++next_id_);
    writer.put(R"("})");
    return writer.frame();
}

std::expected<Protocol::Frame, WsBuildError> Protocol::spot_private_subscribe_frame(std::string_view channel) {
    if (channel.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    FrameWriter writer{frame_};
    writer.put(R"({"action":"sub","ch":")");
    writer.put(channel);
    writer.put(R"("})");
    return writer.frame();
}

} // namespace raikiri::venues::htx
