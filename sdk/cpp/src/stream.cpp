#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <future>
#include <random>

#include "internal.hpp"
#include "sni.hpp"

namespace raikiri::sdk {

namespace wire {

struct Head {
    std::string type{};
    std::optional<std::string> seq{};
};

struct Hello {
    std::string epoch{};
    std::int64_t heartbeat_ms{5000};
};

struct QuotesFrame {
    std::vector<Quote> quotes{};
};

struct BookFrame {
    Book book{};
};

struct GreeksFrame {
    std::vector<Greeks> greeks{};
};

struct FundingFrame {
    std::vector<Funding> funding{};
};

struct TickerFrame {
    std::vector<Ticker> ticker{};
};

struct OrderFrame {
    OrderEvent order_event{};
};

struct FillFrame {
    Fill fill{};
};

struct PositionFrame {
    Position position{};
};

struct BalanceFrame {
    AccountBalances balance{};
};

struct AlgoFrame {
    AlgoOrder algo{};
};

struct AlgoLostFrame {
    AlgoLost algo_lost{};
};

struct GapFrame {
    std::optional<std::string> from{};
    std::optional<std::string> to{};
    std::optional<std::string> reason{};
};

struct ProblemFrame {
    struct Body {
        std::string title{};
        std::int32_t status{0};
        std::optional<std::string> detail{};
        std::string code{};
    };

    Body problem{};
};

struct PrivateSubscribe {
    std::string op{"subscribe"};
    std::string id{"private"};
    std::vector<std::string> channels{};
    std::optional<std::string> resume_from{};
    std::optional<std::string> epoch{};
};

struct MarketSubscribe {
    std::string op{"subscribe"};
    std::string id{};
    std::optional<std::vector<std::string>> quotes{};
    std::optional<std::string> book{};
    std::optional<std::int64_t> levels{};
    std::optional<std::vector<std::string>> greeks{};
    std::optional<std::vector<std::string>> funding{};
    std::optional<std::vector<std::string>> ticker{};
    std::optional<std::int64_t> hz{};
};

} // namespace wire

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

template <typename T> std::string to_json(const T& value) {
    std::string out;
    (void)glz::write<internal::k_write>(value, out);
    return out;
}

template <typename T> bool from_json(T& value, std::string_view text) {
    return !glz::read<internal::k_lenient>(value, text);
}

constexpr std::int64_t k_clock_is_off_ms = 1'000;

std::optional<std::vector<std::string>> maybe(const std::vector<std::string>& ids) {
    return ids.empty() ? std::nullopt : std::optional{ids};
}

} // namespace

struct Stream::Core {
    virtual ~Core() = default;
    virtual void subscribe(const Subscription& subscription) = 0;
    virtual void close() = 0;
    std::atomic<std::uint64_t> last_seq{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<bool> connected{false};
};

namespace {

// the protocol and the reconnect loop; the socket type is the subclass's
class Session : public Stream::Core, public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<internal::IoThread> io,
            internal::Url url,
            std::shared_ptr<internal::Signer> signer,
            StreamHandlers handlers)
        : io_{std::move(io)}, url_{std::move(url)}, signer_{std::move(signer)}, handlers_{std::move(handlers)},
          retry_{io_->context()}, watchdog_{io_->context()} {}

    void start(const Subscription& subscription) {
        add(subscription);
        asio::post(io_->context(), [self = shared_from_this()] { self->open(); });
    }

    void subscribe(const Subscription& subscription) override {
        std::promise<void> applied;
        auto done = applied.get_future();
        auto self = shared_from_this();
        const auto apply = [self, subscription, &applied, inline_call = io_->on_this_thread()] {
            const auto before = self->channels_;
            const auto market = self->add(subscription);
            if (self->connected.load(std::memory_order_relaxed)) {
                if (self->channels_ != before) {
                    self->send(self->private_message());
                }
                if (market.has_value()) {
                    self->send(*market);
                }
            }
            if (!inline_call) {
                applied.set_value();
            }
        };
        if (io_->on_this_thread()) {
            apply();
            return;
        }
        asio::post(io_->context(), apply);
        done.wait();
    }

    void close() override {
        if (closed_.exchange(true)) {
            return;
        }
        std::promise<void> stopped;
        auto done = stopped.get_future();
        const auto on_io = io_->on_this_thread();
        auto self = shared_from_this();
        const auto shut = [self, &stopped, on_io] {
            self->retry_.cancel();
            self->watchdog_.cancel();
            self->drop();
            if (!on_io) {
                stopped.set_value();
            }
        };
        if (on_io) {
            shut();
            return;
        }
        asio::post(io_->context(), shut);
        done.wait();
    }

protected:
    virtual void connect_socket() = 0;
    virtual void drop() = 0;
    virtual void write_text(const std::string& text, std::function<void(beast::error_code)> done) = 0;
    virtual void read_text(beast::flat_buffer& buffer, std::function<void(beast::error_code)> done) = 0;

    [[nodiscard]] std::shared_ptr<internal::IoThread>& io() noexcept { return io_; }

    [[nodiscard]] const internal::Url& url() const noexcept { return url_; }

    [[nodiscard]] std::shared_ptr<Session> session() { return shared_from_this(); }

    [[nodiscard]] internal::Signer::Signed sign_upgrade() {
        return signer_->sign("GET", internal::k_stream_target, {}, {});
    }

    [[nodiscard]] const std::string& key() const noexcept { return signer_->key(); }

    void on_socket_failed(const beast::error_code&, int upgrade_status) {
        if (closed_.load()) {
            return;
        }
        if (upgrade_status == 401 && !resynced_) {
            // a refused upgrade shows no body here, so ask the clock directly; this blocks the thread for one short
            // call, once per stream
            resynced_ = true;
            const auto offset = internal::measure_offset_ms(url_, std::chrono::seconds{2});
            if (offset.has_value() && (*offset > k_clock_is_off_ms || *offset < -k_clock_is_off_ms)) {
                signer_->set_offset_ms(*offset);
                schedule_reconnect();
                return;
            }
        }
        if (upgrade_status == 401 || upgrade_status == 403) {
            // a refusal the next attempt would get as well
            closed_.store(true);
            report({.kind = upgrade_status == 401 ? ErrorKind::Authentication : ErrorKind::PermissionDenied,
                    .status = upgrade_status,
                    .title = "the stream refused this key"});
            return;
        }
        schedule_reconnect();
    }

    void on_socket_open() {
        connected.store(true, std::memory_order_relaxed);
        if (ever_open_) {
            reconnects.fetch_add(1, std::memory_order_relaxed);
        }
        ever_open_ = true;
        attempt_ = 0;
        arm_watchdog();
        read_next();
    }

private:
    std::optional<std::string> add(const Subscription& s) {
        const auto note = [this](bool wanted, const char* name) {
            if (wanted && std::ranges::find(channels_, name) == channels_.end()) {
                channels_.emplace_back(name);
            }
        };
        note(s.algos, "algos");
        note(s.balances, "balances");
        note(s.fills, "fills");
        note(s.orders, "orders");
        note(s.positions, "positions");
        if (s.quotes.empty() && !s.book.has_value() && s.greeks.empty() && s.funding.empty() && s.ticker.empty()) {
            return std::nullopt;
        }
        wire::MarketSubscribe message{.id = "m" + std::to_string(++next_market_),
                                      .quotes = maybe(s.quotes),
                                      .book = s.book,
                                      .greeks = maybe(s.greeks),
                                      .funding = maybe(s.funding),
                                      .ticker = maybe(s.ticker)};
        if (s.book.has_value() && s.levels.has_value()) {
            message.levels = *s.levels;
        }
        if (s.hz.has_value()) {
            message.hz = *s.hz;
        }
        auto text = to_json(message);
        market_.push_back(text);
        return text;
    }

    [[nodiscard]] std::string private_message() const {
        wire::PrivateSubscribe message{.channels = channels_};
        const auto seq = last_seq.load(std::memory_order_relaxed);
        if (seq != 0 && !epoch_.empty()) {
            message.resume_from = std::to_string(seq);
            message.epoch = epoch_;
        }
        return to_json(message);
    }

    void open() {
        if (closed_.load()) {
            return;
        }
        connect_socket();
    }

    void schedule_reconnect() {
        connected.store(false, std::memory_order_relaxed);
        watchdog_.cancel();
        drop();
        if (closed_.load()) {
            return;
        }
        thread_local std::mt19937 generator{std::random_device{}()};
        const auto base = std::min<std::int64_t>(200LL << std::min(attempt_, 8), 30'000);
        std::uniform_int_distribution<std::int64_t> jitter{base / 2, base};
        ++attempt_;
        retry_.expires_after(std::chrono::milliseconds{jitter(generator)});
        retry_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->open();
            }
        });
    }

    void arm_watchdog() {
        watchdog_.expires_after(std::chrono::milliseconds{heartbeat_ms_ * 3});
        watchdog_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec && !self->closed_.load()) {
                self->schedule_reconnect();
            }
        });
    }

    void read_next() {
        auto self = shared_from_this();
        read_text(buffer_, [self](beast::error_code ec) {
            if (ec) {
                self->on_socket_failed(ec, 0);
                return;
            }
            const auto data = self->buffer_.cdata();
            const std::string text{static_cast<const char*>(data.data()), data.size()};
            self->buffer_.consume(self->buffer_.size());
            self->arm_watchdog();
            self->on_frame(text);
            if (!self->closed_.load()) {
                self->read_next();
            }
        });
    }

    void send(std::string text) {
        queue_.push_back(std::move(text));
        pump();
    }

    void pump() {
        if (writing_ || queue_.empty() || !connected.load(std::memory_order_relaxed)) {
            return;
        }
        writing_ = true;
        current_ = std::move(queue_.front());
        queue_.pop_front();
        auto self = shared_from_this();
        write_text(current_, [self](beast::error_code ec) {
            self->writing_ = false;
            if (!ec) {
                self->pump();
            }
        });
    }

    template <typename Fn> void guarded(Fn&& fn) {
        try {
            fn();
        } catch (const std::exception& error) {
            report({.kind = ErrorKind::Config, .title = "a stream handler threw", .detail = error.what()});
        }
    }

    void report(const Error& error) {
        if (handlers_.on_error) {
            handlers_.on_error(error);
        }
    }

    [[nodiscard]] bool fresh_seq(const std::optional<std::string>& text, std::uint64_t& seq) {
        seq = text.has_value() ? std::strtoull(text->c_str(), nullptr, 10) : 0;
        if (seq == 0) {
            return true;
        }
        if (seq <= last_seq.load(std::memory_order_relaxed)) {
            return false;
        }
        last_seq.store(seq, std::memory_order_relaxed);
        return true;
    }

    template <typename Frame, typename Pick, typename Handler>
    void sequenced(std::string_view text, const wire::Head& head, Pick pick, const Handler& handler) {
        std::uint64_t seq = 0;
        if (!fresh_seq(head.seq, seq)) {
            return;
        }
        Frame frame{};
        if (handler && from_json(frame, text)) {
            guarded([&] { handler(pick(frame), seq); });
        }
    }

    void on_frame(std::string_view text) {
        wire::Head head{};
        if (!from_json(head, text)) {
            return;
        }
        if (head.type == "hello") {
            wire::Hello hello{};
            if (from_json(hello, text)) {
                heartbeat_ms_ = std::max<std::int64_t>(hello.heartbeat_ms, 100);
                server_epoch_ = hello.epoch;
                if (epoch_.empty()) {
                    epoch_ = hello.epoch;
                }
            }
            queue_.clear();
            if (!channels_.empty()) {
                send(private_message());
            }
            for (const auto& message : market_) {
                send(message);
            }
            arm_watchdog();
        } else if (head.type == "quotes") {
            wire::QuotesFrame frame{};
            if (handlers_.on_quote && from_json(frame, text)) {
                for (const auto& row : frame.quotes) {
                    guarded([&] { handlers_.on_quote(row); });
                }
            }
        } else if (head.type == "book") {
            wire::BookFrame frame{};
            if (handlers_.on_book && from_json(frame, text)) {
                guarded([&] { handlers_.on_book(frame.book); });
            }
        } else if (head.type == "greeks") {
            wire::GreeksFrame frame{};
            if (handlers_.on_greeks && from_json(frame, text)) {
                for (const auto& row : frame.greeks) {
                    guarded([&] { handlers_.on_greeks(row); });
                }
            }
        } else if (head.type == "funding") {
            wire::FundingFrame frame{};
            if (handlers_.on_funding && from_json(frame, text)) {
                for (const auto& row : frame.funding) {
                    guarded([&] { handlers_.on_funding(row); });
                }
            }
        } else if (head.type == "ticker") {
            wire::TickerFrame frame{};
            if (handlers_.on_ticker && from_json(frame, text)) {
                for (const auto& row : frame.ticker) {
                    guarded([&] { handlers_.on_ticker(row); });
                }
            }
        } else if (head.type == "order") {
            sequenced<wire::OrderFrame>(
                text, head, [](const auto& f) -> const auto& { return f.order_event; }, handlers_.on_order);
        } else if (head.type == "fill") {
            sequenced<wire::FillFrame>(
                text, head, [](const auto& f) -> const auto& { return f.fill; }, handlers_.on_fill);
        } else if (head.type == "position") {
            sequenced<wire::PositionFrame>(
                text, head, [](const auto& f) -> const auto& { return f.position; }, handlers_.on_position);
        } else if (head.type == "balance") {
            sequenced<wire::BalanceFrame>(
                text, head, [](const auto& f) -> const auto& { return f.balance; }, handlers_.on_balance);
        } else if (head.type == "algo") {
            sequenced<wire::AlgoFrame>(
                text, head, [](const auto& f) -> const auto& { return f.algo; }, handlers_.on_algo);
        } else if (head.type == "algo_lost") {
            sequenced<wire::AlgoLostFrame>(
                text, head, [](const auto& f) -> const auto& { return f.algo_lost; }, handlers_.on_algo_lost);
        } else if (head.type == "gap") {
            wire::GapFrame frame{};
            (void)from_json(frame, text);
            Gap gap{.reason = frame.reason.value_or(std::string{})};
            if (frame.from.has_value()) {
                gap.from_seq = std::strtoull(frame.from->c_str(), nullptr, 10);
                if (frame.to.has_value()) {
                    gap.to_seq = std::strtoull(frame.to->c_str(), nullptr, 10);
                }
                // our resume point is void: everything from here on counts in the server's current epoch
                epoch_ = server_epoch_;
                last_seq.store(0, std::memory_order_relaxed);
            } else {
                std::uint64_t seq = 0;
                (void)fresh_seq(head.seq, seq);
            }
            if (handlers_.on_gap) {
                guarded([&] { handlers_.on_gap(gap); });
            }
        } else if (head.type == "problem") {
            wire::ProblemFrame frame{};
            if (from_json(frame, text)) {
                report(internal::error_of_problem(frame.problem.status, to_json(frame.problem), {}, std::nullopt));
            }
        }
    }

    std::shared_ptr<internal::IoThread> io_;
    internal::Url url_;
    std::shared_ptr<internal::Signer> signer_;
    StreamHandlers handlers_;
    asio::steady_timer retry_;
    asio::steady_timer watchdog_;
    beast::flat_buffer buffer_;
    std::deque<std::string> queue_;
    std::string current_;
    bool writing_{false};
    std::vector<std::string> channels_;
    std::vector<std::string> market_;
    int next_market_{0};
    std::string epoch_;
    std::string server_epoch_;
    std::int64_t heartbeat_ms_{5000};
    int attempt_{0};
    bool ever_open_{false};
    bool resynced_{false};
    std::atomic<bool> closed_{false};
};

template <typename Socket> void decorate(Socket& ws, internal::Signer::Signed stamped, std::string key) {
    ws.set_option(websocket::stream_base::decorator(
        [stamped = std::move(stamped), key = std::move(key)](websocket::request_type& request) {
            request.set(internal::k_key_header, key);
            request.set(internal::k_timestamp_header, stamped.timestamp);
            request.set(internal::k_signature_header, stamped.signature);
        }));
    ws.set_option(websocket::stream_base::timeout{.handshake_timeout = std::chrono::seconds{10},
                                                  .idle_timeout = websocket::stream_base::none(),
                                                  .keep_alive_pings = false});
    ws.text(true);
}

class PlainSession final : public Session {
public:
    using Session::Session;

protected:
    void connect_socket() override {
        ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(io()->context());
        resolver_ = std::make_unique<tcp::resolver>(io()->context());
        auto self = std::static_pointer_cast<PlainSession>(session());
        resolver_->async_resolve(
            url().host, url().port, [self](beast::error_code ec, const tcp::resolver::results_type& results) {
                if (ec) {
                    self->on_socket_failed(ec, 0);
                    return;
                }
                beast::get_lowest_layer(*self->ws_).expires_after(std::chrono::seconds{10});
                beast::get_lowest_layer(*self->ws_)
                    .async_connect(results, [self](beast::error_code connect_ec, const tcp::endpoint&) {
                        if (connect_ec) {
                            self->on_socket_failed(connect_ec, 0);
                            return;
                        }
                        beast::get_lowest_layer(*self->ws_).expires_never();
                        decorate(*self->ws_, self->sign_upgrade(), self->key());
                        self->ws_->async_handshake(self->upgrade_,
                                                   self->url().host,
                                                   std::string{internal::k_stream_target},
                                                   [self](beast::error_code shake_ec) {
                                                       if (shake_ec) {
                                                           self->on_socket_failed(
                                                               shake_ec, static_cast<int>(self->upgrade_.result_int()));
                                                           return;
                                                       }
                                                       self->on_socket_open();
                                                   });
                    });
            });
    }

    void drop() override {
        if (ws_ != nullptr) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*ws_).socket().close(ignored);
        }
    }

    void write_text(const std::string& text, std::function<void(beast::error_code)> done) override {
        ws_->async_write(asio::buffer(text), [done = std::move(done)](beast::error_code ec, std::size_t) { done(ec); });
    }

    void read_text(beast::flat_buffer& buffer, std::function<void(beast::error_code)> done) override {
        ws_->async_read(buffer, [done = std::move(done)](beast::error_code ec, std::size_t) { done(ec); });
    }

private:
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    std::unique_ptr<tcp::resolver> resolver_;
    websocket::response_type upgrade_;
};

class TlsSession final : public Session {
public:
    TlsSession(std::shared_ptr<internal::IoThread> io_thread,
               internal::Url address,
               std::shared_ptr<internal::Signer> signer,
               StreamHandlers handlers)
        : Session{std::move(io_thread), std::move(address), std::move(signer), std::move(handlers)},
          tls_{asio::ssl::context::tls_client} {
        tls_.set_default_verify_paths();
        tls_.set_verify_mode(asio::ssl::verify_peer);
    }

protected:
    void connect_socket() override {
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(io()->context(), tls_);
        resolver_ = std::make_unique<tcp::resolver>(io()->context());
        auto self = std::static_pointer_cast<TlsSession>(session());
        if (!internal::set_sni_host(ws_->next_layer().native_handle(), url().host.c_str())) {
            on_socket_failed(beast::error_code{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()},
                             0);
            return;
        }
        ws_->next_layer().set_verify_callback(asio::ssl::host_name_verification{url().host});
        resolver_->async_resolve(
            url().host, url().port, [self](beast::error_code ec, const tcp::resolver::results_type& results) {
                if (ec) {
                    self->on_socket_failed(ec, 0);
                    return;
                }
                beast::get_lowest_layer(*self->ws_).expires_after(std::chrono::seconds{10});
                beast::get_lowest_layer(*self->ws_)
                    .async_connect(results, [self](beast::error_code connect_ec, const tcp::endpoint&) {
                        if (connect_ec) {
                            self->on_socket_failed(connect_ec, 0);
                            return;
                        }
                        self->ws_->next_layer().async_handshake(
                            asio::ssl::stream_base::client, [self](beast::error_code tls_ec) {
                                if (tls_ec) {
                                    self->on_socket_failed(tls_ec, 0);
                                    return;
                                }
                                beast::get_lowest_layer(*self->ws_).expires_never();
                                decorate(*self->ws_, self->sign_upgrade(), self->key());
                                self->ws_->async_handshake(self->upgrade_,
                                                           self->url().host,
                                                           std::string{internal::k_stream_target},
                                                           [self](beast::error_code shake_ec) {
                                                               if (shake_ec) {
                                                                   self->on_socket_failed(
                                                                       shake_ec,
                                                                       static_cast<int>(self->upgrade_.result_int()));
                                                                   return;
                                                               }
                                                               self->on_socket_open();
                                                           });
                            });
                    });
            });
    }

    void drop() override {
        if (ws_ != nullptr) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*ws_).socket().close(ignored);
        }
    }

    void write_text(const std::string& text, std::function<void(beast::error_code)> done) override {
        ws_->async_write(asio::buffer(text), [done = std::move(done)](beast::error_code ec, std::size_t) { done(ec); });
    }

    void read_text(beast::flat_buffer& buffer, std::function<void(beast::error_code)> done) override {
        ws_->async_read(buffer, [done = std::move(done)](beast::error_code ec, std::size_t) { done(ec); });
    }

private:
    asio::ssl::context tls_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    std::unique_ptr<tcp::resolver> resolver_;
    websocket::response_type upgrade_;
};

} // namespace

Stream::Stream() noexcept = default;

Stream::Stream(std::shared_ptr<Core> core) noexcept : core_{std::move(core)} {}

Stream::~Stream() {
    if (core_ != nullptr) {
        core_->close();
    }
}

Stream::Stream(Stream&&) noexcept = default;

Stream& Stream::operator=(Stream&& other) noexcept {
    if (core_ != nullptr && core_ != other.core_) {
        core_->close();
    }
    core_ = std::move(other.core_);
    return *this;
}

void Stream::subscribe(const Subscription& subscription) {
    if (core_ != nullptr) {
        core_->subscribe(subscription);
    }
}

void Stream::close() {
    if (core_ != nullptr) {
        core_->close();
    }
}

std::uint64_t Stream::last_seq() const noexcept {
    return core_ == nullptr ? 0 : core_->last_seq.load(std::memory_order_relaxed);
}

std::uint64_t Stream::reconnects() const noexcept {
    return core_ == nullptr ? 0 : core_->reconnects.load(std::memory_order_relaxed);
}

bool Stream::connected() const noexcept {
    return core_ != nullptr && core_->connected.load(std::memory_order_relaxed);
}

namespace internal {

std::shared_ptr<Stream::Core> open_stream(std::shared_ptr<IoThread> io,
                                          Url url,
                                          std::shared_ptr<Signer> signer,
                                          const Subscription& subscription,
                                          StreamHandlers handlers) {
    if (url.tls) {
        auto session = std::make_shared<TlsSession>(
            std::move(io), std::move(url), std::move(signer), std::move(handlers));
        session->start(subscription);
        return session;
    }
    auto session = std::make_shared<PlainSession>(
        std::move(io), std::move(url), std::move(signer), std::move(handlers));
    session->start(subscription);
    return session;
}

} // namespace internal

} // namespace raikiri::sdk
