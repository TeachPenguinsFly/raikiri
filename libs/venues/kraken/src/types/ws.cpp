#include <raikiri/venues/kraken/endpoints.hpp>
#include <raikiri/venues/kraken/ws.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace raikiri::venues::kraken {

namespace {

// reports overflow rather than truncating a frame
class Writer {
public:
    explicit Writer(std::span<char> into) noexcept : into_{into} {}

    void put(std::string_view text) noexcept {
        if (overflow_ || len_ + text.size() > into_.size()) {
            overflow_ = true;
            return;
        }
        std::memcpy(into_.data() + len_, text.data(), text.size());
        len_ += text.size();
    }

    [[nodiscard]] std::expected<Protocol::Frame, WsBuildError>
    frame(std::size_t consumed) const noexcept {
        if (overflow_) {
            return std::unexpected{WsBuildError::BufferTooSmall};
        }
        return Protocol::Frame{.text = {into_.data(), len_}, .consumed = consumed};
    }

private:
    std::span<char> into_;
    std::size_t len_{0};
    bool overflow_{false};
};

} // namespace

std::string_view to_string(WsBuildError err) noexcept {
    switch (err) {
    case WsBuildError::UnsupportedKind:
        return "UnsupportedKind";
    case WsBuildError::NothingToSend:
        return "NothingToSend";
    case WsBuildError::BufferTooSmall:
        return "BufferTooSmall";
    }
    return "Unknown";
}

std::string_view stream_host() noexcept {
    return ep::k_stream;
}

std::string_view stream_port() noexcept {
    return "443";
}

std::string_view stream_path() noexcept {
    return ep::k_ws;
}

std::string_view feed_for(exec::SubKind kind) noexcept {
    switch (kind) {
    case exec::SubKind::Bbo:
    case exec::SubKind::Funding:
    case exec::SubKind::MarkPrice:
        return "ticker";
    case exec::SubKind::Trades:
        return "trade";
    case exec::SubKind::Depth:
        return "book";
    case exec::SubKind::Kline:
        return {}; // Kraken serves candles over a separate REST charts API
    }
    return {};
}

void add_subscription(std::vector<OwnedSubscription>& set,
                      std::string_view feed,
                      std::string_view product_id) {
    auto run_end = set.end();
    for (auto it = set.begin(); it != set.end(); ++it) {
        if (it->feed != feed) {
            continue;
        }
        if (it->product_id == product_id) {
            return;
        }
        run_end = std::next(it);
    }
    set.insert(run_end,
               OwnedSubscription{.feed = std::string{feed}, .product_id = std::string{product_id}});
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::event_frame(std::string_view event, std::span<const Subscription> subscriptions) {
    if (subscriptions.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    const auto feed = subscriptions.front().feed;
    if (feed.empty()) {
        return std::unexpected{WsBuildError::UnsupportedKind};
    }
    std::size_t batch = 0;
    while (batch < subscriptions.size() &&
           subscriptions[batch].feed == feed) {
        if (subscriptions[batch].product_id.empty()) {
            return std::unexpected{WsBuildError::UnsupportedKind};
        }
        ++batch;
    }

    Writer writer{frame_};
    writer.put(R"({"event":")");
    writer.put(event);
    writer.put(R"(","feed":")");
    writer.put(feed);
    writer.put(R"(","product_ids":[)");
    for (std::size_t i = 0; i < batch; ++i) {
        if (i != 0) {
            writer.put(",");
        }
        writer.put("\"");
        writer.put(subscriptions[i].product_id);
        writer.put("\"");
    }
    writer.put("]}");
    return writer.frame(batch);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::subscribe_frame(std::span<const Subscription> subscriptions) {
    return event_frame("subscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::unsubscribe_frame(std::span<const Subscription> subscriptions) {
    return event_frame("unsubscribe", subscriptions);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::challenge_request_frame(std::string_view api_key) {
    if (api_key.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    Writer writer{frame_};
    writer.put(R"({"event":"challenge","api_key":")");
    writer.put(api_key);
    writer.put(R"("})");
    return writer.frame(0);
}

std::expected<Protocol::Frame, WsBuildError>
Protocol::private_subscribe_frame(std::string_view feed,
                                  std::string_view api_key,
                                  std::string_view original_challenge,
                                  std::string_view signed_challenge) {
    // an empty credential would subscribe the public form, which kraken answers with an alert
    if (feed.empty() || api_key.empty() || original_challenge.empty() || signed_challenge.empty()) {
        return std::unexpected{WsBuildError::NothingToSend};
    }
    Writer writer{frame_};
    writer.put(R"({"event":"subscribe","feed":")");
    writer.put(feed);
    writer.put(R"(","api_key":")");
    writer.put(api_key);
    writer.put(R"(","original_challenge":")");
    writer.put(original_challenge);
    writer.put(R"(","signed_challenge":")");
    writer.put(signed_challenge);
    writer.put(R"("})");
    return writer.frame(0);
}

} // namespace raikiri::venues::kraken
