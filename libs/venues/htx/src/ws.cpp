#include <raikiri/venues/htx/ws.hpp>

#include <algorithm>
#include <boost/beast/zlib/inflate_stream.hpp>
#include <charconv>
#include <cstring>
#include <span>
#include <vector>

namespace raikiri::venues::htx {

namespace {

namespace zlib = boost::beast::zlib;

constexpr std::string_view k_host = "api.hbdm.com";

// The largest observed inflated frame is a step0 book at about 4 KiB; the
// ceiling stops a broken or hostile stream from making the gateway allocate
// without bound.
constexpr std::size_t k_max_inflated = std::size_t{8} * 1024 * 1024;

constexpr std::size_t k_initial_inflated = std::size_t{64} * 1024;

// RFC 1952 fixed header: magic, method, flags, mtime, extra flags, OS.
constexpr std::size_t k_gzip_header_bytes = 10;
constexpr unsigned char k_gzip_magic_0 = 0x1F;
constexpr unsigned char k_gzip_magic_1 = 0x8B;
constexpr unsigned char k_gzip_deflate = 0x08;
constexpr unsigned char k_flag_extra = 0x04;
constexpr unsigned char k_flag_name = 0x08;
constexpr unsigned char k_flag_comment = 0x10;
constexpr unsigned char k_flag_header_crc = 0x02;

// Skips the gzip wrapper and hands back the raw deflate stream inside it.
// Beast's inflate_stream is a raw-deflate decoder — and the only zlib in the
// dependency set, since boost is header-only and nothing else links one — so
// the container comes off by hand.
//
// The 8-byte CRC32/ISIZE trailer is left on the end; inflate stops at the
// final block and ignores it. TLS already authenticates every byte that
// reaches here, and a corrupt payload fails at the JSON parse a moment later.
std::expected<std::string_view, InflateError> strip_gzip_header(std::string_view frame) {
    if (frame.size() < k_gzip_header_bytes) {
        return std::unexpected{InflateError::Truncated};
    }
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(frame[i]); };
    if (byte(0) != k_gzip_magic_0 || byte(1) != k_gzip_magic_1) {
        return std::unexpected{InflateError::NotGzip};
    }
    if (byte(2) != k_gzip_deflate) {
        return std::unexpected{InflateError::NotGzip};
    }
    const auto flags = byte(3);
    std::size_t at = k_gzip_header_bytes;
    if ((flags & k_flag_extra) != 0) {
        if (at + 2 > frame.size()) {
            return std::unexpected{InflateError::Truncated};
        }
        const std::size_t extra = byte(at) | (static_cast<std::size_t>(byte(at + 1)) << 8U);
        at += 2 + extra;
    }
    for (const auto flag : {k_flag_name, k_flag_comment}) {
        if ((flags & flag) == 0) {
            continue;
        }
        const auto end = frame.find('\0', at);
        if (end == std::string_view::npos) {
            return std::unexpected{InflateError::Truncated};
        }
        at = end + 1;
    }
    if ((flags & k_flag_header_crc) != 0) {
        at += 2;
    }
    if (at >= frame.size()) {
        return std::unexpected{InflateError::Truncated};
    }
    return frame.substr(at);
}

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

struct Inflater::Impl {
    zlib::inflate_stream stream;
    std::vector<char> out;
};

Inflater::Inflater() : impl_{std::make_unique<Impl>()} {
    impl_->out.resize(k_initial_inflated);
}

Inflater::~Inflater() = default;

std::expected<std::string_view, InflateError> Inflater::inflate(std::string_view frame) {
    const auto deflated = strip_gzip_header(frame);
    if (!deflated.has_value()) {
        return std::unexpected{deflated.error()};
    }

    // Each frame is an independent stream; reset() drops the last one's window
    // without dropping the allocation.
    impl_->stream.reset();

    zlib::z_params params{};
    params.next_in = deflated->data();
    params.avail_in = deflated->size();
    std::size_t produced = 0;
    while (true) {
        params.next_out = impl_->out.data() + produced;
        params.avail_out = impl_->out.size() - produced;
        boost::system::error_code ec;
        impl_->stream.write(params, zlib::Flush::sync, ec);
        produced = impl_->out.size() - params.avail_out;
        if (ec == zlib::error::end_of_stream) {
            return std::string_view{impl_->out.data(), produced};
        }
        if (ec && ec != zlib::error::need_buffers) {
            return std::unexpected{InflateError::Corrupt};
        }
        // need_buffers with room left means the input ended mid-stream;
        // growing again would spin forever.
        if (params.avail_out != 0) {
            return std::unexpected{InflateError::Corrupt};
        }
        if (impl_->out.size() >= k_max_inflated) {
            return std::unexpected{InflateError::TooLarge};
        }
        impl_->out.resize(std::min(impl_->out.size() * 2, k_max_inflated));
    }
}

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view to_string(InflateError err) noexcept {
    switch (err) {
    case InflateError::NotGzip:
        return "NotGzip";
    case InflateError::Truncated:
        return "Truncated";
    case InflateError::TooLarge:
        return "TooLarge";
    case InflateError::Corrupt:
        return "Corrupt";
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
        // Top of book on its own topic, so no local book has to be maintained
        // to produce a BBO.
        return "bbo";
    case exec::SubKind::Trades:
        return "trade.detail";
    case exec::SubKind::Depth:
        // step0 is unaggregated; step1..step15 merge levels and would report a
        // tick size the instrument does not have.
        return "depth.step0";
    // Funding is a different socket and MarkPrice a kline-shaped series on a
    // third one (/ws_index, VERIFIED); neither is named here.
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
