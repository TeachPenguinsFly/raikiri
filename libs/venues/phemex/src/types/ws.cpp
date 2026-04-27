#include <raikiri/venues/phemex/ws.hpp>

#include <charconv>
#include <cstring>

namespace raikiri::venues::phemex {

namespace {

std::string_view join(std::string_view channel, std::string_view symbol, std::span<char> out) {
    if (symbol.empty()) {
        return channel;
    }
    const auto size = channel.size() + 1 + symbol.size();
    if (size > out.size()) {
        return {};
    }
    std::memcpy(out.data(), channel.data(), channel.size());
    out[channel.size()] = ':';
    std::memcpy(out.data() + channel.size() + 1, symbol.data(), symbol.size());
    return {out.data(), size};
}

} // namespace

std::string_view channel_key(exec::SubKind kind,
                             MarketType market,
                             Encoding encoding,
                             std::string_view symbol,
                             std::span<char> out) noexcept {
    const bool real = encoding == Encoding::Real;
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Depth:
        return join(real ? k_real_book : k_scaled_book, symbol, out);
    case exec::SubKind::Trades:
        return join(real ? k_real_trade : k_scaled_trade, symbol, out);
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        if (market == MarketType::Spot) {
            return k_spot_ticker;
        }
        return real ? k_perp_pack : k_inverse_ticker;
    case exec::SubKind::Kline:
        return {};
    }
    return {};
}

std::string_view key_symbol(std::string_view key) noexcept {
    const auto colon = key.find(':');
    return colon == std::string_view::npos ? std::string_view{} : key.substr(colon + 1);
}

bool is_book_key(std::string_view key) noexcept {
    const auto channel = key.substr(0, key.find(':'));
    return channel == k_real_book || channel == k_scaled_book;
}

std::string_view Protocol::subscribe_frame(std::string_view key) {
    const auto colon = key.find(':');
    const auto channel = key.substr(0, colon);
    const auto symbol = colon == std::string_view::npos ? std::string_view{} : key.substr(colon + 1);

    std::array<char, 24> id{};
    const auto [id_end, ec] = std::to_chars(id.data(), id.data() + id.size(), next_id_);
    if (ec != std::errc{}) {
        return {};
    }
    const std::string_view id_text{id.data(), static_cast<std::size_t>(id_end - id.data())};

    constexpr std::string_view k_open = R"({"id":)";
    constexpr std::string_view k_method = R"(,"method":")";
    constexpr std::string_view k_params = R"(.subscribe","params":[)";
    constexpr std::string_view k_close = "]}";
    const std::string_view quote = symbol.empty() ? std::string_view{} : std::string_view{"\""};
    const std::array parts{k_open, id_text, k_method, channel, k_params, quote, symbol, quote, k_close};
    std::size_t size = 0;
    for (const auto part : parts) {
        size += part.size();
    }
    if (size > frame_.size()) {
        return {};
    }
    auto* cursor = frame_.data();
    for (const auto part : parts) {
        if (!part.empty()) {
            std::memcpy(cursor, part.data(), part.size());
            cursor += part.size();
        }
    }
    ++next_id_;
    return {frame_.data(), size};
}

std::string_view Protocol::ping_frame() noexcept {
    return R"({"id":0,"method":"server.ping","params":[]})";
}

} // namespace raikiri::venues::phemex
