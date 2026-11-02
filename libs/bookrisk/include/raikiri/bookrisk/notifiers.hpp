#pragma once

#include <raikiri/bookrisk/ports.hpp>
#include <raikiri/config/secret.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace raikiri::bookrisk {

class LogNotifier final : public Notifier {
public:
    void notify(const Notice& notice) override;
};

class FanOutNotifier final : public Notifier {
public:
    void add(Notifier& sink) { sinks_.push_back(&sink); }

    void notify(const Notice& notice) override;

private:
    std::vector<Notifier*> sinks_{};
};

// true when delivered
using WebhookPost = std::function<bool(std::string_view url, std::string_view body)>;

// posts over tls with no retries; the loop is what a slow endpoint must not hold up
[[nodiscard]] WebhookPost make_https_post();

// warnings and worse go out on their own thread, oldest dropped past a short queue; steady lines stay in the log
class WebhookNotifier final : public Notifier {
public:
    static constexpr std::size_t k_queue = 64;

    WebhookNotifier(std::string url, WebhookPost post);
    ~WebhookNotifier() override;

    void notify(const Notice& notice) override;

private:
    void run(const std::stop_token& stop);

    std::string url_;
    WebhookPost post_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::string> queue_;
    std::jthread worker_;
};

// resolves a credential reference to the url; refuses one that carries the url in the config file
[[nodiscard]] std::expected<std::unique_ptr<WebhookNotifier>, std::string>
make_webhook(std::string_view reference, config::SecretProvider& secrets, WebhookPost post);

} // namespace raikiri::bookrisk
