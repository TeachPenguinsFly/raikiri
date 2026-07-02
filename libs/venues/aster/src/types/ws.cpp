#include <raikiri/venues/aster/endpoints.hpp>
#include <raikiri/venues/aster/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::aster {

namespace {

[[nodiscard]] constexpr char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string_view suffix_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return k_bbo_stream;
    case exec::SubKind::Trades:
        return k_trade_stream;
    case exec::SubKind::Depth:
        return k_depth_stream;
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return k_mark_price_stream;
    case exec::SubKind::Kline:
        return {}; // needs an interval; not wired until a strategy wants it
    }
    return {};
}

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::TooManyStreams:
        return "TooManyStreams";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host(bool testnet) noexcept {
    return ep::stream_host(testnet);
}

std::string_view stream_path() noexcept {
    return ep::k_ws_combined;
}

std::string_view
stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept {
    const auto suffix = suffix_for(kind);
    if (suffix.empty() || exchange_symbol.empty()) {
        return {};
    }
    if (exchange_symbol.size() + suffix.size() > out.size()) {
        return {};
    }
    std::size_t len = 0;
    for (const char c : exchange_symbol) {
        out[len++] = lower(c);
    }
    std::memcpy(out.data() + len, suffix.data(), suffix.size());
    len += suffix.size();
    return {out.data(), len};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::method_frame(std::string_view method, std::span<const std::string_view> streams) {
    if (streams.empty()) {
        return std::unexpected{WsBuildError::TooManyStreams};
    }
    const auto batch = streams.size();
    for (std::size_t i = 0; i < batch; ++i) {
        if (streams[i].empty()) {
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
    put(R"({"method":")");
    put(method);
    put(R"(","params":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            put(",");
        }
        put("\"");
        put(streams[i]);
        put("\"");
    }
    put(R"(],"id":)");
    std::array<char, 24> id_buf{};
    const auto id = next_id_++;
    const auto [stop, ec] = std::to_chars(id_buf.data(), id_buf.data() + id_buf.size(), id);
    if (ec != std::errc{}) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    put({id_buf.data(), static_cast<std::size_t>(stop - id_buf.data())});
    put("}");
    if (overflow) {
        return std::unexpected{WsBuildError::BufferTooSmall};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const std::string_view> streams) {
    return method_frame("SUBSCRIBE", streams);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const std::string_view> streams) {
    return method_frame("UNSUBSCRIBE", streams);
}

} // namespace raikiri::venues::aster
