#include <raikiri/venues/bingx/ws.hpp>

#include <algorithm>
#include <boost/beast/zlib/inflate_stream.hpp>
#include <charconv>
#include <cstring>
#include <vector>

namespace raikiri::venues::bingx {

namespace {

namespace zlib = boost::beast::zlib;

constexpr std::string_view k_host = "open-api-swap.bingx.com";

// The largest observed inflated frame is a few kilobytes; the ceiling stops a
// broken or hostile stream from making the gateway allocate without bound.
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
// The 8-byte CRC32/ISIZE trailer is left on the end; inflate stops at the final
// block and ignores it. TLS already authenticates every byte that reaches here,
// and a corrupt payload fails at the JSON parse a moment later.
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

std::string_view stream_path() noexcept {
    return "/swap-market";
}

std::string_view data_type_suffix(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return "bookTicker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        // 100 is BingX's deepest tier: depth200 is refused outright with
        // 80015 "dataType not support" rather than downgraded.
        return "depth100@100ms";
    case exec::SubKind::MarkPrice:
        return "markPrice";
    case exec::SubKind::Funding:
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::request(std::string_view req_type, std::string_view symbol, std::string_view suffix) {
    if (symbol.empty() || suffix.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
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

    std::array<char, 24> id{};
    const auto [stop, ec] = std::to_chars(id.data(), id.data() + id.size(), ++next_id_);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put(R"({"id":")");
    put(std::string_view{id.data(), static_cast<std::size_t>(stop - id.data())});
    put(R"(","reqType":")");
    put(req_type);
    put(R"(","dataType":")");
    put(symbol);
    put("@");
    put(suffix);
    put(R"("})");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}};
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::string_view symbol,
                                                                       std::string_view suffix) {
    return request("sub", symbol, suffix);
}

std::expected<Protocol::Frame, WsBuildError> Protocol::unsubscribe_frame(std::string_view symbol,
                                                                         std::string_view suffix) {
    return request("unsub", symbol, suffix);
}

} // namespace raikiri::venues::bingx
