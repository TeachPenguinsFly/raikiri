#include <raikiri/venues/backpack/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::backpack {

std::string_view stream_prefix(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
        return k_bbo_stream;
    case exec::SubKind::Trades:
        return k_trade_stream;
    case exec::SubKind::Depth:
        return k_depth_stream;
    case exec::SubKind::MarkPrice:
        return k_mark_price_stream;
    case exec::SubKind::Funding: // decided by the caller, which knows the market
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::string_view stream_name(exec::SubKind kind, std::string_view exchange_symbol, std::span<char> out) noexcept {
    const auto prefix = stream_prefix(kind);
    if (prefix.empty() || exchange_symbol.empty() || prefix.size() + exchange_symbol.size() > out.size()) {
        return {};
    }
    std::memcpy(out.data(), prefix.data(), prefix.size());
    std::memcpy(out.data() + prefix.size(), exchange_symbol.data(), exchange_symbol.size());
    return {out.data(), prefix.size() + exchange_symbol.size()};
}

std::string_view depth_symbol(std::string_view key) noexcept {
    return key.starts_with(k_depth_stream) ? key.substr(k_depth_stream.size()) : std::string_view{};
}

std::expected<Protocol::Frame, std::string_view> Protocol::subscribe_frame(std::span<const std::string_view> streams) {
    if (streams.empty()) {
        return std::unexpected{"no streams"};
    }
    const auto batch = streams.size();
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
    put(R"({"method":"SUBSCRIBE","params":[)");
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
    const auto [stop, ec] = std::to_chars(id_buf.data(), id_buf.data() + id_buf.size(), next_id_++);
    if (ec != std::errc{}) {
        return std::unexpected{"id does not render"};
    }
    put({id_buf.data(), static_cast<std::size_t>(stop - id_buf.data())});
    put("}");
    if (overflow) {
        return std::unexpected{"frame buffer too small"};
    }
    return Frame{.text = {frame_.data(), len}, .consumed = batch};
}

} // namespace raikiri::venues::backpack
