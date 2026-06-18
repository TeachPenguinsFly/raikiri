#include <raikiri/venues/lighter/budget.hpp>
#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace raikiri::venues::lighter {

namespace {

constexpr std::uint64_t k_minute_ns = 60ULL * 1'000'000'000ULL;

[[nodiscard]] std::uint32_t burst_of(std::uint32_t per_minute) noexcept {
    return std::max<std::uint32_t>(1U, per_minute / 30U);
}

} // namespace

MessageBudget::MessageBudget(std::uint32_t per_minute) noexcept
    : per_minute_{std::max<std::uint32_t>(1U, per_minute)}, burst_{burst_of(per_minute_)},
      limiter_{burst_, (k_minute_ns * burst_) / per_minute_} {}

bool MessageBudget::take(MonoNanos now) noexcept {
    const std::scoped_lock lock{mu_};
    return limiter_.take(1, now).has_value();
}

void MessageBudget::hold(MonoNanos now, std::uint64_t hold_ns) noexcept {
    const std::scoped_lock lock{mu_};
    limiter_.penalize(now, hold_ns);
}

std::uint32_t ws_messages_per_minute() noexcept {
    const std::string name{ep::k_ws_messages_env};
    const char* raw = std::getenv(name.c_str());
    if (raw == nullptr) {
        return ep::k_ws_messages_per_minute;
    }
    std::uint32_t value = 0;
    const auto* end = raw + std::strlen(raw);
    const auto [stop, ec] = std::from_chars(raw, end, value);
    if (ec != std::errc{} || stop != end || value == 0) {
        return ep::k_ws_messages_per_minute;
    }
    return value;
}

std::shared_ptr<MessageBudget> process_budget() {
    static const auto budget = std::make_shared<MessageBudget>(ws_messages_per_minute());
    return budget;
}

bool Outbox::admit(std::string_view frame, MonoNanos now) {
    if (!budget_->take(now)) {
        return false;
    }
    if (const auto channel = subscribed_channel(frame); !channel.empty()) {
        unanswered_.emplace_back(channel);
    }
    return true;
}

void Outbox::answered(std::string_view venue_channel) {
    const auto colon = venue_channel.rfind(':');
    if (colon == std::string_view::npos) {
        return;
    }
    const auto head = venue_channel.substr(0, colon);
    const auto tail = venue_channel.substr(colon + 1);
    const auto found = std::ranges::find_if(unanswered_, [&](const std::string& ours) {
        return ours.size() == venue_channel.size() && ours.starts_with(head) &&
               ours[head.size()] == '/' && ours.ends_with(tail);
    });
    if (found != unanswered_.end()) {
        unanswered_.erase(found);
    }
}

std::optional<std::string> Outbox::refused(std::int64_t code, MonoNanos now) {
    if (code == ep::k_ws_rate_limited) {
        budget_->hold(now, ep::k_ws_refusal_hold_ns);
    }
    if (unanswered_.empty()) {
        return std::nullopt;
    }
    auto oldest = std::move(unanswered_.front());
    unanswered_.pop_front();
    if (code != ep::k_ws_rate_limited) {
        return std::nullopt;
    }
    return oldest;
}

} // namespace raikiri::venues::lighter
