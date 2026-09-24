#include <raikiri/net/inflater.hpp>

#include <algorithm>
#include <boost/beast/zlib/inflate_stream.hpp>
#include <vector>

namespace raikiri::net {

namespace {

namespace zlib = boost::beast::zlib;

// a ceiling against a hostile stream; frames are a few kilobytes
constexpr std::size_t k_max_inflated = std::size_t{8} * 1024 * 1024;

constexpr std::size_t k_initial_inflated = std::size_t{64} * 1024;

// rfc 1952 fixed header: magic, method, flags, mtime, extra flags, os
constexpr std::size_t k_gzip_header_bytes = 10;
constexpr unsigned char k_gzip_magic_0 = 0x1F;
constexpr unsigned char k_gzip_magic_1 = 0x8B;
constexpr unsigned char k_gzip_deflate = 0x08;
constexpr unsigned char k_flag_extra = 0x04;
constexpr unsigned char k_flag_name = 0x08;
constexpr unsigned char k_flag_comment = 0x10;
constexpr unsigned char k_flag_header_crc = 0x02;

// strips the gzip wrapper for beast's raw-deflate decoder; the crc trailer is ignored
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

} // namespace raikiri::net
