#include <raikiri/bookrisk/notifiers.hpp>
#include <raikiri/log/log.hpp>
#include <raikiri/net/io_context.hpp>
#include <raikiri/net/rest_client.hpp>

#include <glaze/glaze.hpp>
#include <optional>

namespace raikiri::bookrisk {

struct WebhookBody {
    std::string level{};
    std::string kind{};
    std::string subject{};
    std::string text{};
    std::string at{};
};

void LogNotifier::notify(const Notice& notice) {
    switch (notice.level) {
    case NoticeLevel::Steady:
        RLOG_INFO("risk[{}] {}: {}", notice.subject, notice.kind, notice.text);
        return;
    case NoticeLevel::Warning:
        RLOG_WARN("risk[{}] {}: {}", notice.subject, notice.kind, notice.text);
        return;
    case NoticeLevel::Critical:
        RLOG_ERROR("risk[{}] {}: {}", notice.subject, notice.kind, notice.text);
        return;
    }
}

void FanOutNotifier::notify(const Notice& notice) {
    for (auto* sink : sinks_) {
        sink->notify(notice);
    }
}

WebhookNotifier::WebhookNotifier(std::string url, WebhookPost post)
    : url_{std::move(url)}, post_{std::move(post)}, worker_{[this](const std::stop_token& stop) { run(stop); }} {}

WebhookNotifier::~WebhookNotifier() {
    worker_.request_stop();
    ready_.notify_all();
}

void WebhookNotifier::notify(const Notice& notice) {
    if (notice.level == NoticeLevel::Steady) {
        return;
    }
    const WebhookBody body{.level = notice.level == NoticeLevel::Critical ? "critical" : "warning",
                           .kind = std::string{notice.kind},
                           .subject = std::string{notice.subject},
                           .text = std::string{notice.text},
                           .at = std::to_string(notice.at.ns)};
    std::string json;
    if (glz::write_json(body, json)) {
        return;
    }
    {
        const std::scoped_lock hold{mutex_};
        if (queue_.size() == k_queue) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(json));
    }
    ready_.notify_one();
}

void WebhookNotifier::run(const std::stop_token& stop) {
    std::unique_lock hold{mutex_};
    while (ready_.wait(hold, stop, [this] { return !queue_.empty(); })) {
        auto body = std::move(queue_.front());
        queue_.pop_front();
        hold.unlock();
        if (!post_(url_, body)) {
            RLOG_WARN("risk: a notice could not be delivered to the webhook");
        }
        hold.lock();
    }
}

WebhookPost make_https_post() {
    return [](std::string_view url, std::string_view body) {
        constexpr std::string_view k_scheme = "https://";
        if (!url.starts_with(k_scheme)) {
            return false;
        }
        const auto rest = url.substr(k_scheme.size());
        const auto slash = rest.find('/');
        const auto authority = rest.substr(0, slash);
        const auto target = slash == std::string_view::npos ? std::string_view{"/"} : rest.substr(slash);
        const auto colon = authority.find(':');
        net::IoContext io;
        net::RestClient client{
            io,
            {.host = std::string{authority.substr(0, colon)},
             .port = colon == std::string_view::npos ? std::string{"443"} : std::string{authority.substr(colon + 1)},
             .pool_size = 1}};
        client.start();
        std::optional<bool> done;
        const bool sent = client.send(
            {.method = net::HttpMethod::Post, .target = target, .body = body},
            [&done](net::RestResponse&& answer) { done = answer.status >= 200 && answer.status < 300; });
        for (std::uint64_t waited = 0; sent && !done.has_value() && waited < client.max_wait_ms(); waited += 50) {
            (void)io.run_for(50);
        }
        client.stop();
        (void)io.poll();
        return done.value_or(false);
    };
}

std::expected<std::unique_ptr<WebhookNotifier>, std::string>
make_webhook(std::string_view reference, config::SecretProvider& secrets, WebhookPost post) {
    const auto ref = config::SecretRef::parse(reference);
    if (!ref.has_value()) {
        return std::unexpected{"notify_webhook is not a credential reference"};
    }
    if (config::is_plaintext(ref->scheme)) {
        return std::unexpected{"notify_webhook must be a reference, not the url itself"};
    }
    config::Secret url;
    if (const auto resolved = secrets.resolve(*ref, url); !resolved.has_value()) {
        return std::unexpected{std::string{"notify_webhook: "} + std::string{config::to_string(resolved.error())}};
    }
    return std::make_unique<WebhookNotifier>(std::string{url.view()}, std::move(post));
}

} // namespace raikiri::bookrisk
