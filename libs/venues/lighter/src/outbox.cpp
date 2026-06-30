#include <raikiri/venues/lighter/endpoints.hpp>
#include <raikiri/venues/lighter/outbox.hpp>
#include <raikiri/venues/lighter/ws.hpp>

#include <algorithm>

namespace raikiri::venues::lighter {

bool Outbox::admit(std::string_view frame, MonoNanos) {
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

std::optional<std::string> Outbox::refused(std::int64_t code, MonoNanos) {
    if (code == ep::k_ws_rate_limited) {
        meters_->hold_messages(net::steady_ns(), ep::k_ws_refusal_hold_ns);
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
