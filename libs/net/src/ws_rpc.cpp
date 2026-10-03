#include <raikiri/net/ws_rpc.hpp>

#include <utility>

namespace raikiri::net {

std::string_view to_string(WsRpcState state) noexcept {
    switch (state) {
    case WsRpcState::Down:
        return "down";
    case WsRpcState::LoggingIn:
        return "logging_in";
    case WsRpcState::Ready:
        return "ready";
    case WsRpcState::AuthFailed:
        return "auth_failed";
    }
    return "unknown";
}

WsRpc::WsRpc(IoContext& io, const Clock& clock, WsRpcConfig config, WsRpcCodec& codec)
    : clock_{&clock}, config_{std::move(config)}, codec_{&codec} {
    config_.ws.keep_queue_on_close = false;
    ws_ = std::make_unique<WsClient>(io, config_.ws, *this);
    pending_.reserve(config_.max_in_flight);
}

WsRpc::WsRpc(const Clock& clock, WsRpcConfig config, WsRpcCodec& codec, Link link)
    : clock_{&clock}, config_{std::move(config)}, codec_{&codec}, link_{std::move(link)} {
    pending_.reserve(config_.max_in_flight);
}

// the owner is being destroyed, so nothing calls back into it
WsRpc::~WsRpc() {
    observer_ = nullptr;
    pending_.clear();
    ws_.reset();
}

void WsRpc::start() {
    running_ = true;
    if (ws_) {
        ws_->start();
    }
}

void WsRpc::stop() {
    running_ = false;
    if (ws_) {
        ws_->stop();
    }
    fail_all("channel stopped with the request in flight");
    enter(WsRpcState::Down, {});
}

void WsRpc::set_observer(Observer observer) {
    observer_ = std::move(observer);
}

void WsRpc::set_admit(Admit admit) {
    admit_ = std::move(admit);
}

bool WsRpc::write(std::string_view frame) {
    if (ws_) {
        return ws_->connected() && ws_->send(frame);
    }
    return link_ && link_(frame);
}

void WsRpc::enter(WsRpcState state, std::string_view detail) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (observer_) {
        observer_(state, detail);
    }
}

bool WsRpc::request(std::uint64_t id, std::string_view frame, WsRpcCallback on_reply) {
    if (state_ != WsRpcState::Ready || pending_.size() >= config_.max_in_flight) {
        return false;
    }
    if (admit_ && !admit_()) {
        return false;
    }
    if (!write(frame)) {
        return false;
    }
    ++stats_.sent;
    pending_.push_back(
        {.id = id, .deadline_ns = clock_->mono().ns + config_.request_timeout_ns, .on_reply = std::move(on_reply)});
    return true;
}

void WsRpc::fail_all(std::string_view detail) {
    auto lost = std::exchange(pending_, {});
    pending_.reserve(config_.max_in_flight);
    for (auto& entry : lost) {
        ++stats_.unknown;
        entry.on_reply({.outcome = WsRpcOutcome::Unknown, .payload = {}, .detail = detail});
    }
}

void WsRpc::restart() {
    if (ws_) {
        ws_->stop();
    }
    fail_all("socket closed with the request in flight");
    enter(WsRpcState::Down, {});
    if (ws_ && running_) {
        ws_->start();
    }
}

void WsRpc::poll() noexcept {
    const auto now = clock_->mono().ns;
    for (std::size_t i = 0; i < pending_.size();) {
        if (pending_[i].deadline_ns > now) {
            ++i;
            continue;
        }
        auto expired = std::move(pending_[i]);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        ++stats_.unknown;
        expired.on_reply({.outcome = WsRpcOutcome::Unknown, .payload = {}, .detail = "no reply by the deadline"});
    }
    if (state_ == WsRpcState::LoggingIn && now - login_sent_ns_ > config_.login_timeout_ns) {
        ++stats_.login_failures;
        restart();
        return;
    }
    if (state_ == WsRpcState::AuthFailed && running_ && now >= retry_at_ns_) {
        enter(WsRpcState::Down, {});
        if (ws_) {
            ws_->start();
        }
        return;
    }
    if (state_ == WsRpcState::Ready && config_.ping_interval_ns != 0 && now >= next_ping_ns_) {
        const auto ping = codec_->ping();
        if (!ping.empty() && write(ping)) {
            ping_sent_ns_ = now;
        }
        next_ping_ns_ = now + config_.ping_interval_ns;
    }
}

void WsRpc::on_open() {
    const auto frame = codec_->login(clock_->wall());
    if (!frame.has_value()) {
        ++stats_.login_failures;
        retry_at_ns_ = clock_->mono().ns + config_.login_retry_ns;
        enter(WsRpcState::AuthFailed, "no credentials to log in with");
        return;
    }
    next_ping_ns_ = clock_->mono().ns + config_.ping_interval_ns;
    if (frame->empty() && !codec_->challenged()) {
        ++stats_.logins;
        enter(WsRpcState::Ready, {});
        return;
    }
    login_sent_ns_ = clock_->mono().ns;
    enter(WsRpcState::LoggingIn, {});
    if (!frame->empty() && !write(*frame)) {
        restart();
    }
}

void WsRpc::on_message(std::string_view payload, UnixNanos) noexcept {
    const auto frame = codec_->classify(payload);
    switch (frame.kind) {
    case WsRpcFrame::Kind::Reply: {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].id != frame.id) {
                continue;
            }
            auto answered = std::move(pending_[i]);
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats_.answered;
            answered.on_reply({.outcome = WsRpcOutcome::Answered, .payload = payload, .detail = {}});
            return;
        }
        // late after its deadline, or never ours
        ++stats_.unmatched;
        return;
    }
    case WsRpcFrame::Kind::Login:
        if (state_ != WsRpcState::LoggingIn) {
            return;
        }
        if (frame.ok) {
            ++stats_.logins;
            stats_.login_rtt_ns = clock_->mono().ns - login_sent_ns_;
            enter(WsRpcState::Ready, {});
            return;
        }
        ++stats_.login_failures;
        retry_at_ns_ = clock_->mono().ns + config_.login_retry_ns;
        if (ws_) {
            ws_->stop();
        }
        enter(WsRpcState::AuthFailed, frame.detail);
        return;
    case WsRpcFrame::Kind::Challenge: {
        if (state_ != WsRpcState::LoggingIn) {
            return;
        }
        const auto reply = codec_->answer(payload);
        if (!reply.has_value() || !write(*reply)) {
            restart();
        }
        return;
    }
    case WsRpcFrame::Kind::Pong:
        if (ping_sent_ns_ != 0) {
            stats_.ping_rtt_ns = clock_->mono().ns - ping_sent_ns_;
            ping_sent_ns_ = 0;
        }
        return;
    case WsRpcFrame::Kind::Other:
        return;
    }
}

void WsRpc::on_close(WsCloseReason) {
    fail_all("socket closed with the request in flight");
    if (state_ != WsRpcState::AuthFailed) {
        enter(WsRpcState::Down, {});
    }
}

} // namespace raikiri::net
