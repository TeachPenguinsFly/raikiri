#include <raikiri/net/ws_limits.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace raikiri::net {
namespace {

[[nodiscard]] std::uint32_t share_percent(std::string_view venue) noexcept {
    std::string name{"RAIKIRI_"};
    for (const char c : venue) {
        name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    name += "_WS_SHARE";
    const char* raw = std::getenv(name.c_str());
    if (raw == nullptr) {
        return 100;
    }
    std::uint32_t value = 0;
    const auto* end = raw + std::strlen(raw);
    const auto [stop, ec] = std::from_chars(raw, end, value);
    return ec == std::errc{} && stop == end && value >= 1 && value <= 100 ? value : 100;
}

template <typename T> [[nodiscard]] T scaled(T value, std::uint32_t percent) noexcept {
    return std::max<T>(1, static_cast<T>(value * percent / 100));
}

WsLimits g_limits;
std::unique_ptr<IpMeters> g_meters = std::make_unique<IpMeters>(WsLimits{});

} // namespace

WsLimits
ws_limits(std::string_view venue, const config::VenueSettings& settings, std::string_view market) {
    const auto share = share_percent(venue);
    WsLimits out;
    for (const auto& limit : settings.limits) {
        if (!limit.kind.starts_with("ws_") || (!limit.market.empty() && limit.market != market)) {
            continue;
        }
        out.configured = true;
        const WsRate rate{.max = limit.max, .window_ns = limit.window_ms * 1'000'000ULL};
        if (limit.kind == "ws_streams") {
            out.streams_per_socket = limit.max;
        } else if (limit.kind == "ws_sockets") {
            out.sockets = scaled<std::size_t>(limit.max, share);
        } else if (limit.kind == "ws_frame") {
            out.subscriptions_per_frame = limit.max;
        } else if (limit.kind == "ws_channels") {
            out.channels = scaled<std::size_t>(limit.max, share);
        } else if (limit.kind == "ws_connects") {
            out.connects = {.max = scaled(rate.max, share), .window_ns = rate.window_ns};
        } else if (limit.kind == "ws_messages" && limit.scope == "socket") {
            out.socket_messages = rate;
        } else if (limit.kind == "ws_messages") {
            out.ip_messages.push_back({.max = scaled(rate.max, share), .window_ns = rate.window_ns});
        }
    }
    return out;
}

std::uint64_t Meter::take(std::uint64_t now_ns) noexcept {
    const std::scoped_lock lock{mu_};
    const auto wait = log_.wait_ns(1, now_ns);
    if (wait == 0) {
        log_.spend(1, now_ns);
    }
    return wait;
}

std::uint64_t Meter::wait(std::uint64_t now_ns) noexcept {
    const std::scoped_lock lock{mu_};
    return log_.wait_ns(1, now_ns);
}

void Meter::hold(std::uint64_t now_ns, std::uint64_t hold_ns) noexcept {
    const std::scoped_lock lock{mu_};
    log_.hold(now_ns, hold_ns);
}

IpMeters::IpMeters(const WsLimits& limits) : channels_max_{limits.channels} {
    for (const auto& rate : limits.ip_messages) {
        messages_.push_back(std::make_unique<Meter>(rate));
    }
    if (limits.connects.max != 0) {
        connects_ = std::make_unique<Meter>(limits.connects);
    }
}

bool IpMeters::take_message(std::uint64_t now_ns) noexcept {
    if (std::ranges::any_of(messages_, [now_ns](const auto& m) { return m->wait(now_ns) != 0; })) {
        return false;
    }
    for (const auto& meter : messages_) {
        (void)meter->take(now_ns);
    }
    return true;
}

void IpMeters::hold_messages(std::uint64_t now_ns, std::uint64_t hold_ns) noexcept {
    for (const auto& meter : messages_) {
        meter->hold(now_ns, hold_ns);
    }
}

std::uint64_t IpMeters::connect(std::uint64_t now_ns) noexcept {
    return connects_ != nullptr ? connects_->take(now_ns) : 0;
}

std::size_t IpMeters::claim_channels(std::size_t wanted) noexcept {
    const std::scoped_lock lock{mu_};
    if (channels_max_ == 0) {
        return wanted;
    }
    const auto granted = std::min(wanted, channels_max_ - std::min(channels_max_, channels_used_));
    channels_used_ += granted;
    return granted;
}

void set_process_ws_limits(const WsLimits& limits) {
    g_limits = limits;
    g_meters = std::make_unique<IpMeters>(limits);
}

std::size_t frame_subscriptions() noexcept {
    return std::max<std::size_t>(1, g_limits.subscriptions_per_frame);
}

const WsLimits& process_ws_limits() noexcept {
    return g_limits;
}

IpMeters& process_ip_meters() noexcept {
    return *g_meters;
}

std::uint64_t steady_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace raikiri::net
