#include <raikiri/venues/cryptocom/endpoints.hpp>
#include <raikiri/venues/cryptocom/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace raikiri::venues::cryptocom {

namespace {

class Appender {
public:
    explicit Appender(std::span<char> buf) noexcept : buf_{buf} {}

    Appender& operator<<(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > buf_.size()) {
            overflow_ = true;
            return *this;
        }
        text.copy(buf_.data() + len_, text.size());
        len_ += text.size();
        return *this;
    }

    Appender& operator<<(std::int64_t value) noexcept {
        std::array<char, 24> num{};
        const auto [stop, ec] = std::to_chars(num.data(), num.data() + num.size(), value);
        if (ec != std::errc{}) {
            overflow_ = true;
            return *this;
        }
        return *this << std::string_view{num.data(), static_cast<std::size_t>(stop - num.data())};
    }

    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::span<char> buf_;
    std::size_t len_{0};
    bool overflow_{false};
};

constexpr std::string_view k_tail = R"(],"book_subscription_type":"SNAPSHOT_AND_UPDATE","book_update_frequency":)";

} // namespace

std::string_view channel_name(std::string_view prefix, std::string_view symbol, std::span<char> out) noexcept {
    Appender text{out};
    text << prefix << symbol;
    return text.ok() ? text.view() : std::string_view{};
}

std::string_view book_channel(std::string_view symbol, std::span<char> out) noexcept {
    Appender text{out};
    text << k_book_prefix << symbol << "." << static_cast<std::int64_t>(k_book_depth);
    return text.ok() ? text.view() : std::string_view{};
}

std::string_view instrument_of(std::string_view channel) noexcept {
    const auto dot = channel.find('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    auto rest = channel.substr(dot + 1);
    if (channel.starts_with(k_book_prefix)) {
        const auto depth = rest.rfind('.');
        if (depth == std::string_view::npos) {
            return {};
        }
        rest = rest.substr(0, depth);
    }
    return rest;
}

std::expected<Protocol::Frame, WsBuildError> Protocol::subscribe_frame(std::span<const std::string_view> channels) {
    std::array<char, 8> cadence{};
    const auto [stop, ec] = std::to_chars(cadence.data(), cadence.data() + cadence.size(), k_book_update_ms);
    (void)ec;
    const std::string_view cadence_text{cadence.data(), static_cast<std::size_t>(stop - cadence.data())};
    const auto closing = k_tail.size() + cadence_text.size() + 2;
    const auto limit = std::min(frame_.size(), ep::k_max_frame_bytes);

    Appender text{std::span<char>{frame_.data(), limit}};
    text << R"({"id":)" << static_cast<std::int64_t>(next_id_) << R"(,"method":"subscribe","params":{"channels":[)";
    std::size_t consumed = 0;
    for (const auto channel : channels) {
        const auto cost = channel.size() + 2 + (consumed == 0 ? 0 : 1);
        if (text.size() + cost + closing > limit) {
            break;
        }
        if (consumed != 0) {
            text << ",";
        }
        text << "\"" << channel << "\"";
        ++consumed;
    }
    if (consumed == 0 && !channels.empty()) {
        return std::unexpected(WsBuildError::TooLarge);
    }
    text << k_tail << cadence_text << "}}";
    if (!text.ok()) {
        return std::unexpected(WsBuildError::TooLarge);
    }
    ++next_id_;
    return Frame{.text = text.view(), .consumed = consumed};
}

std::string_view Protocol::heartbeat_reply(std::int64_t id) {
    Appender text{reply_};
    text << R"({"id":)" << id << R"(,"method":"public/respond-heartbeat"})";
    return text.ok() ? text.view() : std::string_view{};
}

} // namespace raikiri::venues::cryptocom
