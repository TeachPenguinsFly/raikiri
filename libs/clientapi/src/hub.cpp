#include <raikiri/clientapi/application/hub.hpp>

#include <algorithm>

namespace raikiri::clientapi {

bool visible_to(const ApiKey& key, const PrivateEvent& event) noexcept {
    if (!event.account.empty() && !allows_account(key, event.account)) {
        return false;
    }
    return event.symbol.empty() || allows_symbol(key, event.symbol);
}

void StreamHub::publish(const std::string& client_id,
                        std::uint8_t channels,
                        std::string_view account,
                        std::string_view symbol,
                        PrivatePayload payload) {
    const std::scoped_lock lock{mutex_};
    publish_locked(client_id, rings_[client_id], channels, account, symbol, std::move(payload));
}

void StreamHub::publish_connected(std::uint8_t channels, std::string_view account, const PrivatePayload& payload) {
    const std::scoped_lock lock{mutex_};
    for (auto& [client_id, ring] : rings_) {
        std::erase_if(ring.subscribers, [](const auto& weak) { return weak.expired(); });
        if (!ring.subscribers.empty()) {
            publish_locked(client_id, ring, channels, account, {}, payload);
        }
    }
}

void StreamHub::publish_locked(const std::string&,
                               Ring& ring,
                               std::uint8_t channels,
                               std::string_view account,
                               std::string_view symbol,
                               PrivatePayload payload) {
    auto event = std::make_shared<PrivateEvent>();
    event->seq = ++ring.seq;
    event->channels = channels;
    event->account = account;
    event->symbol = symbol;
    event->payload = std::move(payload);
    ring.events.push_back(event);
    if (ring.events.size() > capacity_) {
        ring.events.pop_front();
    }
    std::erase_if(ring.subscribers, [&event](const auto& weak) {
        const auto live = weak.lock();
        if (!live) {
            return true;
        }
        live->deliver(event);
        return false;
    });
}

void StreamHub::attach(const std::string& client_id, std::weak_ptr<StreamSubscriber> subscriber) {
    const std::scoped_lock lock{mutex_};
    rings_[client_id].subscribers.push_back(std::move(subscriber));
}

ResumePlan StreamHub::since(const std::string& client_id, std::string_view epoch, std::uint64_t from) {
    const std::scoped_lock lock{mutex_};
    auto& ring = rings_[client_id];
    ResumePlan plan{.head = ring.seq};
    if (epoch != epoch_) {
        // this ring started over, so the client's number says nothing about what it missed
        plan.gap = true;
        plan.gap_from = from + 1;
        return plan;
    }
    if (from >= ring.seq) {
        return plan;
    }
    const auto oldest = ring.events.empty() ? ring.seq + 1 : ring.events.front()->seq;
    if (from + 1 < oldest) {
        plan.gap = true;
        plan.gap_from = from + 1;
        plan.gap_to = oldest - 1;
    }
    for (const auto& event : ring.events) {
        if (event->seq > from) {
            plan.events.push_back(event);
        }
    }
    return plan;
}

std::uint64_t StreamHub::head(const std::string& client_id) {
    const std::scoped_lock lock{mutex_};
    return rings_[client_id].seq;
}

bool StreamHub::any_connected() {
    const std::scoped_lock lock{mutex_};
    return std::ranges::any_of(rings_, [](auto& entry) {
        std::erase_if(entry.second.subscribers, [](const auto& weak) { return weak.expired(); });
        return !entry.second.subscribers.empty();
    });
}

bool StreamHub::acquire_stream(const ApiKey& key) {
    const std::scoped_lock lock{mutex_};
    auto& open = streams_[key.id];
    if (open >= key.max_streams) {
        return false;
    }
    ++open;
    return true;
}

void StreamHub::release_stream(const std::string& key_id) {
    const std::scoped_lock lock{mutex_};
    if (const auto found = streams_.find(key_id); found != streams_.end() && found->second > 0) {
        --found->second;
    }
}

} // namespace raikiri::clientapi
