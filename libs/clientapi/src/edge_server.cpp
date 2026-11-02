#include <raikiri/clientapi/infrastructure/edge_server.hpp>
#include <raikiri/clientapi/presentation/frames.hpp>
#include <raikiri/log/log.hpp>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace raikiri::clientapi {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::uint64_t k_max_body_bytes = std::uint64_t{64} * 1024;
constexpr std::uint32_t k_max_header_bytes = 16 * 1024;
constexpr std::size_t k_max_ws_message = std::size_t{64} * 1024;
constexpr std::size_t k_market_drop_bytes = std::size_t{1} * 1024 * 1024;
constexpr std::size_t k_close_bytes = std::size_t{16} * 1024 * 1024;
constexpr std::size_t k_max_market_subscriptions = 64;
constexpr auto k_http_timeout = seconds{30};
constexpr auto k_link_retry = seconds{1};
constexpr auto k_parked_wait = milliseconds{20};

using FeedItem = UpstreamFrame;

std::string_view header_of(const http::request<http::string_body>& request, std::string_view name) {
    const auto found = request.find(name);
    return found == request.end() ? std::string_view{} : std::string_view{found->value()};
}

// the rightmost hop is the one the terminator itself appended; anything left of it is the caller's claim
std::string_view last_hop(std::string_view forwarded) noexcept {
    const auto comma = forwarded.rfind(',');
    auto hop = comma == std::string_view::npos ? forwarded : forwarded.substr(comma + 1);
    while (!hop.empty() && hop.front() == ' ') {
        hop.remove_prefix(1);
    }
    return hop;
}

} // namespace

struct EdgeServer::Impl : std::enable_shared_from_this<EdgeServer::Impl> {
    Impl(EdgeOptions opts, ApiRouter& r, Backend& b, StreamHub& h, PrivateFeed& f, WallClock w)
        : options{std::move(opts)}, router{&r}, backend{&b}, hub{&h}, feed{&f}, wall{std::move(w)}, ioc{1},
          workers{options.workers}, acceptor{ioc} {}

    void accept();
    void feed_loop();
    void push_feed(UpstreamFrame frame);

    EdgeOptions options;
    ApiRouter* router;
    Backend* backend;
    StreamHub* hub;
    PrivateFeed* feed;
    WallClock wall;
    asio::io_context ioc;
    asio::thread_pool workers;
    tcp::acceptor acceptor;
    std::thread reactor;
    std::thread feed_thread;
    std::atomic<bool> running{false};
    std::mutex feed_mutex;
    std::condition_variable feed_wake;
    std::deque<UpstreamFrame> feed_queue;
    std::shared_ptr<class UpstreamLink> private_link;
};

using ServerImpl = EdgeServer::Impl;

// one websocket to admind's stream; it reconnects until stopped
class UpstreamLink : public std::enable_shared_from_this<UpstreamLink> {
public:
    using OnFrame = std::function<void(std::string_view)>;
    using OnOpen = std::function<void()>;

    UpstreamLink(ServerImpl& impl, OnFrame on_frame, OnOpen on_open, OnOpen on_lost)
        : impl_{&impl}, on_frame_{std::move(on_frame)}, on_open_{std::move(on_open)}, on_lost_{std::move(on_lost)},
          retry_{impl.ioc} {}

    void start() {
        auto self = shared_from_this();
        asio::post(impl_->workers, [self] {
            auto token = self->impl_->backend->stream_token();
            asio::post(self->impl_->ioc, [self, token = std::move(token)] {
                if (self->stopped_) {
                    return;
                }
                if (!token.has_value()) {
                    self->schedule_retry();
                    return;
                }
                self->connect(*token);
            });
        });
    }

    void send(std::string text) {
        out_.push_back(std::move(text));
        pump();
    }

    void stop() {
        stopped_ = true;
        retry_.cancel();
        if (ws_ != nullptr) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*ws_).socket().close(ignored);
        }
    }

private:
    void connect(const std::string& token) {
        ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(impl_->ioc);
        open_ = false;
        writing_ = false;
        out_.clear();
        beast::error_code ec;
        const auto address = asio::ip::make_address(impl_->options.admin_host, ec);
        if (ec) {
            schedule_retry();
            return;
        }
        auto self = shared_from_this();
        beast::get_lowest_layer(*ws_).expires_after(seconds{5});
        beast::get_lowest_layer(*ws_).async_connect(
            tcp::endpoint{address, impl_->options.admin_stream_port}, [self, token](beast::error_code connect_ec) {
                if (connect_ec || self->stopped_) {
                    self->schedule_retry();
                    return;
                }
                beast::get_lowest_layer(*self->ws_).expires_never();
                self->ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
                self->ws_->async_handshake(
                    self->impl_->options.admin_host, "/v1/stream?token=" + token, [self](beast::error_code shake_ec) {
                        if (shake_ec || self->stopped_) {
                            self->schedule_retry();
                            return;
                        }
                        self->open_ = true;
                        self->on_open_();
                        self->read();
                    });
            });
    }

    void read() {
        auto self = shared_from_this();
        ws_->async_read(buffer_, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                self->open_ = false;
                if (!self->stopped_) {
                    self->on_lost_();
                }
                self->schedule_retry();
                return;
            }
            const auto data = self->buffer_.cdata();
            self->on_frame_({static_cast<const char*>(data.data()), data.size()});
            self->buffer_.consume(self->buffer_.size());
            self->read();
        });
    }

    void pump() {
        if (!open_ || writing_ || out_.empty()) {
            return;
        }
        writing_ = true;
        current_ = std::move(out_.front());
        out_.pop_front();
        auto self = shared_from_this();
        ws_->text(true);
        ws_->async_write(asio::buffer(current_), [self](beast::error_code ec, std::size_t) {
            self->writing_ = false;
            if (!ec) {
                self->pump();
            }
        });
    }

    void schedule_retry() {
        if (stopped_) {
            return;
        }
        retry_.expires_after(k_link_retry);
        auto self = shared_from_this();
        retry_.async_wait([self](beast::error_code ec) {
            if (!ec && !self->stopped_) {
                self->start();
            }
        });
    }

    ServerImpl* impl_;
    OnFrame on_frame_;
    OnOpen on_open_;
    OnOpen on_lost_;
    asio::steady_timer retry_;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::string> out_;
    std::string current_;
    bool open_{false};
    bool writing_{false};
    bool stopped_{false};
};

namespace {

class StreamSession final : public StreamSubscriber, public std::enable_shared_from_this<StreamSession> {
public:
    StreamSession(beast::tcp_stream stream, ServerImpl& impl, std::shared_ptr<const ApiKey> key, std::string peer)
        : ws_{std::move(stream)}, impl_{&impl}, key_{std::move(key)}, peer_{std::move(peer)}, heartbeat_{impl.ioc} {}

    ~StreamSession() override {
        impl_->hub->release_stream(key_->id);
        if (market_ != nullptr) {
            market_->stop();
        }
    }

    void accept(http::request<http::string_body> upgrade) {
        upgrade_ = std::move(upgrade);
        beast::get_lowest_layer(ws_).expires_never();
        websocket::stream_base::timeout timeouts{};
        timeouts.handshake_timeout = seconds{10};
        timeouts.idle_timeout = seconds{30};
        timeouts.keep_alive_pings = true;
        ws_.set_option(timeouts);
        ws_.read_message_max(k_max_ws_message);
        auto self = shared_from_this();
        ws_.async_accept(upgrade_, [self](beast::error_code ec) {
            if (ec) {
                return;
            }
            self->impl_->hub->attach(self->key_->client_id, std::weak_ptr<StreamSubscriber>{self});
            self->last_sent_ = self->impl_->hub->head(self->key_->client_id);
            self->enqueue(hello_frame(self->impl_->hub->epoch(),
                                      self->last_sent_,
                                      self->impl_->options.heartbeat_ms,
                                      self->key_->client_id),
                          false);
            self->beat();
            self->read();
        });
    }

    void deliver(std::shared_ptr<const PrivateEvent> event) override {
        asio::post(impl_->ioc, [self = shared_from_this(), event = std::move(event)] { self->on_event(*event); });
    }

private:
    void on_event(const PrivateEvent& event) {
        if (closed_ || event.seq <= last_sent_) {
            return;
        }
        // a gap reaches every subscriber; anything else only the channels asked for, within the key's scope
        const auto wanted = event.channels == 0 ? mask_ != 0 : (event.channels & mask_) != 0;
        if (!wanted || !visible_to(*key_, event)) {
            return;
        }
        last_sent_ = event.seq;
        enqueue(private_frame(event), false);
    }

    void beat() {
        heartbeat_.expires_after(milliseconds{impl_->options.heartbeat_ms});
        auto self = shared_from_this();
        heartbeat_.async_wait([self](beast::error_code ec) {
            if (ec || self->closed_) {
                return;
            }
            auto current = self->impl_->router->current_key(self->key_->id);
            if (current == nullptr) {
                // revoked by a reload: an open socket must not outlive the key
                self->enqueue(
                    stream_problem_frame({}, {.status = 401, .code = "key-revoked", .title = "this key was revoked"}),
                    false);
                self->shut_after_flush();
                return;
            }
            // the release of the old one's stream slot goes by id, which a reload does not change
            self->key_ = std::move(current);
            self->enqueue(heartbeat_frame(self->impl_->wall(), self->impl_->hub->head(self->key_->client_id)), false);
            self->beat();
        });
    }

    void read() {
        auto self = shared_from_this();
        ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                self->shut();
                return;
            }
            const auto data = self->buffer_.cdata();
            const std::string text{static_cast<const char*>(data.data()), data.size()};
            self->buffer_.consume(self->buffer_.size());
            self->on_message(text);
        });
    }

    void on_message(std::string_view text) {
        auto op = parse_client_op(text);
        if (!op.has_value()) {
            enqueue(stream_problem_frame({}, op.error()), false);
            read();
            return;
        }
        if (op->op == "ping") {
            enqueue(pong_frame(op->id), false);
            read();
            return;
        }
        if (op->op == "unsubscribe") {
            unsubscribe(op->id);
            read();
            return;
        }
        if (op->instruments.empty()) {
            subscribe_private(*op);
            read();
            return;
        }
        // a scope check can ask the backend about a listing, so it leaves the reactor; reading waits for it
        auto self = shared_from_this();
        asio::post(impl_->workers, [self, key = key_, op = *std::move(op)]() mutable {
            auto checked = self->impl_->router->check_market(*key, op.instruments);
            asio::post(self->impl_->ioc, [self, op = std::move(op), checked = std::move(checked)] {
                if (checked.has_value()) {
                    self->subscribe_market(op);
                } else {
                    self->enqueue(stream_problem_frame(op.id, checked.error()), false);
                }
                if (op.channels != 0) {
                    self->subscribe_private(op);
                }
                self->read();
            });
        });
    }

    void subscribe_private(const ClientOp& op) {
        const auto added = static_cast<std::uint8_t>(op.channels & ~mask_);
        mask_ |= op.channels;
        private_id_ = op.id;
        std::uint64_t head = last_sent_;
        if (op.resume_from.has_value()) {
            auto plan = impl_->hub->since(key_->client_id, op.epoch, *op.resume_from);
            head = plan.head;
            if (plan.gap) {
                enqueue(gap_frame(plan.gap_from, plan.gap_to, "these events are gone; refetch over rest"), false);
            }
            for (const auto& event : plan.events) {
                const auto wanted = event->channels == 0 || (event->channels & (added | mask_)) != 0;
                if (wanted && visible_to(*key_, *event)) {
                    enqueue(private_frame(*event), false);
                }
                last_sent_ = std::max(last_sent_, event->seq);
            }
        }
        enqueue(ack_frame(op.id, head), false);
    }

    void subscribe_market(const ClientOp& op) {
        if (market_subs_.size() >= k_max_market_subscriptions) {
            enqueue(stream_problem_frame(op.id,
                                         {.status = 429,
                                          .code = "too-many-subscriptions",
                                          .title = "this stream has every subscription it may hold"}),
                    false);
            return;
        }
        market_subs_.emplace_back(op.id, op.market_message);
        if (market_ == nullptr) {
            std::weak_ptr<StreamSession> weak = shared_from_this();
            market_ = std::make_shared<UpstreamLink>(
                *impl_,
                [weak](std::string_view frame) {
                    if (const auto self = weak.lock()) {
                        self->enqueue(std::string{frame}, true);
                    }
                },
                [weak] {
                    if (const auto self = weak.lock()) {
                        for (const auto& [id, message] : self->market_subs_) {
                            self->market_->send(message);
                        }
                    }
                },
                [] {});
            market_->start();
            return;
        }
        market_->send(op.market_message);
    }

    void unsubscribe(const std::string& id) {
        if (id == private_id_) {
            mask_ = 0;
            enqueue(ack_frame(id, last_sent_), false);
            return;
        }
        const auto found = std::ranges::find_if(market_subs_, [&id](const auto& sub) { return sub.first == id; });
        if (found == market_subs_.end()) {
            enqueue(stream_problem_frame(id, {.status = 404, .code = "not-found", .title = "no such subscription"}),
                    false);
            return;
        }
        market_subs_.erase(found);
        market_->send(market_unsubscribe(id));
    }

    // market frames are the current value of something, so a slow reader loses those first
    void enqueue(std::string frame, bool droppable) {
        if (closed_) {
            return;
        }
        if (droppable && queued_bytes_ > k_market_drop_bytes) {
            return;
        }
        if (queued_bytes_ + frame.size() > k_close_bytes) {
            // it comes back and resumes from its last sequence
            shut();
            return;
        }
        queued_bytes_ += frame.size();
        queue_.push_back(std::move(frame));
        pump();
    }

    void pump() {
        if (writing_ || queue_.empty() || closed_) {
            if (!writing_ && queue_.empty() && shut_when_flushed_) {
                shut();
            }
            return;
        }
        writing_ = true;
        current_ = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= current_.size();
        ws_.text(true);
        auto self = shared_from_this();
        ws_.async_write(asio::buffer(current_), [self](beast::error_code ec, std::size_t) {
            self->writing_ = false;
            if (ec) {
                self->shut();
                return;
            }
            self->pump();
        });
    }

    void shut_after_flush() {
        shut_when_flushed_ = true;
        pump();
    }

    void shut() {
        if (closed_) {
            return;
        }
        closed_ = true;
        heartbeat_.cancel();
        queue_.clear();
        queued_bytes_ = 0;
        beast::error_code ignored;
        (void)beast::get_lowest_layer(ws_).socket().close(ignored);
        if (market_ != nullptr) {
            market_->stop();
        }
    }

    websocket::stream<beast::tcp_stream> ws_;
    ServerImpl* impl_;
    std::shared_ptr<const ApiKey> key_;
    std::string peer_;
    asio::steady_timer heartbeat_;
    http::request<http::string_body> upgrade_;
    beast::flat_buffer buffer_;
    std::deque<std::string> queue_;
    std::size_t queued_bytes_{0};
    std::string current_;
    bool writing_{false};
    bool closed_{false};
    bool shut_when_flushed_{false};
    std::uint8_t mask_{0};
    std::uint64_t last_sent_{0};
    std::string private_id_;
    std::shared_ptr<UpstreamLink> market_;
    std::vector<std::pair<std::string, std::string>> market_subs_;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, ServerImpl& impl) : stream_{std::move(socket)}, impl_{&impl} {
        beast::error_code ec;
        const auto remote = stream_.socket().remote_endpoint(ec);
        if (!ec) {
            auto address = remote.address();
            if (address.is_v6() && address.to_v6().is_v4_mapped()) {
                address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
            }
            peer_ = address.to_string();
        }
    }

    void run() { read(); }

private:
    void read() {
        parser_ = std::make_unique<http::request_parser<http::string_body>>();
        parser_->body_limit(k_max_body_bytes);
        parser_->header_limit(k_max_header_bytes);
        stream_.expires_after(k_http_timeout);
        auto self = shared_from_this();
        http::async_read(stream_, buffer_, *parser_, [self](beast::error_code ec, std::size_t) {
            if (ec == http::error::body_limit || ec == http::error::header_limit) {
                self->write_refusal(413, "the request is larger than this api takes");
                return;
            }
            if (ec) {
                self->close();
                return;
            }
            self->dispatch();
        });
    }

    [[nodiscard]] EdgeRequest view() {
        const auto& message = parser_->get();
        const auto target = std::string_view{message.target()};
        const auto split = target.find('?');
        peer_seen_ = peer_;
        if (in_any_range(impl_->options.trusted_proxies, peer_)) {
            if (const auto forwarded = header_of(message, "X-Forwarded-For"); !forwarded.empty()) {
                peer_seen_ = std::string{last_hop(forwarded)};
            }
        }
        return {.method = std::string_view{message.method_string()},
                .path = target.substr(0, split),
                .query = split == std::string_view::npos ? std::string_view{} : target.substr(split + 1),
                .body = std::string_view{message.body()},
                .key_id = header_of(message, k_key_header),
                .timestamp = header_of(message, k_timestamp_header),
                .signature = header_of(message, k_signature_header),
                .idempotency_key = header_of(message, "Idempotency-Key"),
                .peer_ip = peer_seen_};
    }

    void dispatch() {
        const auto request = view();
        const auto is_stream = websocket::is_upgrade(parser_->get()) && request.path == k_stream_path;
        stream_.expires_never();
        auto self = shared_from_this();
        if (is_stream) {
            asio::post(impl_->workers, [self, request] {
                auto authorized = self->impl_->router->authorize_stream(request);
                asio::post(self->impl_->ioc, [self, authorized = std::move(authorized)]() mutable {
                    if (!authorized.has_value()) {
                        self->write(std::move(authorized).error(), false);
                        return;
                    }
                    std::make_shared<StreamSession>(
                        std::move(self->stream_), *self->impl_, *std::move(authorized), self->peer_seen_)
                        ->accept(self->parser_->release());
                });
            });
            return;
        }
        asio::post(impl_->workers, [self, request] {
            auto response = self->impl_->router->handle(request);
            asio::post(self->impl_->ioc, [self, response = std::move(response)]() mutable {
                self->write(std::move(response), self->parser_->get().keep_alive());
            });
        });
    }

    void write_refusal(int status, std::string_view title) {
        write({.status = status,
               .content_type = "application/problem+json",
               .body = problem_body({.status = status, .code = "too-large", .title = std::string{title}}, {})},
              false);
    }

    void write(EdgeResponse answer, bool keep_alive) {
        auto response = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(answer.status),
                                                                            11);
        response->set(http::field::content_type, answer.content_type);
        response->set(http::field::cache_control, "no-store");
        for (const auto& [name, value] : answer.headers) {
            response->set(name, value);
        }
        response->body() = std::move(answer.body);
        response->prepare_payload();
        response->keep_alive(keep_alive);
        stream_.expires_after(k_http_timeout);
        auto self = shared_from_this();
        http::async_write(stream_, *response, [self, response](beast::error_code ec, std::size_t) {
            if (ec || !response->keep_alive()) {
                self->close();
                return;
            }
            self->read();
        });
    }

    void close() {
        beast::error_code ignored;
        (void)stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
    }

    beast::tcp_stream stream_;
    ServerImpl* impl_;
    beast::flat_buffer buffer_;
    std::unique_ptr<http::request_parser<http::string_body>> parser_;
    std::string peer_;
    std::string peer_seen_;
};

} // namespace

void EdgeServer::Impl::accept() {
    acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<HttpSession>(std::move(socket), *this)->run();
        }
        if (running.load(std::memory_order_relaxed)) {
            accept();
        }
    });
}

void EdgeServer::Impl::push_feed(UpstreamFrame frame) {
    {
        const std::scoped_lock lock{feed_mutex};
        feed_queue.push_back(std::move(frame));
    }
    feed_wake.notify_one();
}

void EdgeServer::Impl::feed_loop() {
    auto due = std::chrono::steady_clock::now();
    std::deque<UpstreamFrame> batch;
    while (running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock lock{feed_mutex};
            feed_wake.wait_until(
                lock, due, [this] { return !feed_queue.empty() || !running.load(std::memory_order_relaxed); });
            batch.swap(feed_queue);
        }
        for (auto& frame : batch) {
            if (frame.gap.has_value()) {
                feed->on_gap(*frame.gap);
            }
            if (frame.order_event.has_value()) {
                feed->on_order_event(*std::move(frame.order_event));
            }
            if (frame.fill.has_value()) {
                feed->on_fill(*std::move(frame.fill));
            }
        }
        batch.clear();
        // an event that outran the worker recording its order waits a moment, not a whole tick
        if (feed->has_parked()) {
            due = std::min(due, std::chrono::steady_clock::now() + k_parked_wait);
        }
        if (std::chrono::steady_clock::now() >= due) {
            due = std::chrono::steady_clock::now() + std::chrono::nanoseconds{feed->tick()};
        }
    }
}

EdgeServer::EdgeServer(
    EdgeOptions options, ApiRouter& router, Backend& backend, StreamHub& hub, PrivateFeed& feed, WallClock wall)
    : impl_{std::make_shared<Impl>(std::move(options), router, backend, hub, feed, std::move(wall))} {}

EdgeServer::~EdgeServer() {
    stop();
}

bool EdgeServer::start() {
    beast::error_code ec;
    const auto address = asio::ip::make_address(impl_->options.address, ec);
    if (ec) {
        return false;
    }
    const tcp::endpoint endpoint{address, impl_->options.port};
    (void)impl_->acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        return false;
    }
    (void)impl_->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    (void)impl_->acceptor.bind(endpoint, ec);
    if (ec) {
        return false;
    }
    (void)impl_->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return false;
    }
    impl_->options.port = impl_->acceptor.local_endpoint(ec).port();
    impl_->running.store(true, std::memory_order_relaxed);
    impl_->accept();

    auto* raw = impl_.get();
    impl_->private_link = std::make_shared<UpstreamLink>(
        *impl_,
        [raw](std::string_view text) {
            if (auto frame = parse_upstream_frame(text); frame.has_value()) {
                raw->push_feed(*std::move(frame));
            }
        },
        [raw] {
            RLOG_INFO("apid: private feed linked to the backend's stream");
            raw->private_link->send(private_link_subscribe());
        },
        [raw] {
            RLOG_WARN("apid: lost the backend's stream; reconnecting");
            raw->push_feed({.gap = "the link to the backend dropped"});
        });
    impl_->private_link->start();
    impl_->reactor = std::thread{[raw] { raw->ioc.run(); }};
    impl_->feed_thread = std::thread{[raw] { raw->feed_loop(); }};
    return true;
}

void EdgeServer::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->feed_wake.notify_all();
    if (impl_->feed_thread.joinable()) {
        impl_->feed_thread.join();
    }
    asio::post(impl_->ioc, [raw = impl_.get()] {
        beast::error_code ignored;
        (void)raw->acceptor.close(ignored);
        raw->private_link->stop();
    });
    impl_->workers.join();
    impl_->ioc.stop();
    if (impl_->reactor.joinable()) {
        impl_->reactor.join();
    }
}

std::uint16_t EdgeServer::port() const noexcept {
    return impl_->options.port;
}

} // namespace raikiri::clientapi
