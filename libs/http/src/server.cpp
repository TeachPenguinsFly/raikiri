#include <raikiri/http/server.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace raikiri::http {

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// bounded so a request cannot make the process allocate
constexpr std::uint64_t k_max_body_bytes = std::uint64_t{16} * 1024;

std::string_view reason_of(int status) noexcept {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

Response failed() {
    return {.status = 500,
            .content_type = "application/problem+json",
            .body = R"({"title":"handler failed","status":500})"};
}

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const DeferredHandler& handler) : stream_{std::move(socket)}, handler_{&handler} {}

    void run() { read(); }

    // an answer given after the handler returned, from whichever thread gave it
    void answer_later(Response answer) {
        boost::asio::post(stream_.get_executor(), [self = shared_from_this(), answer = std::move(answer)]() mutable {
            self->write(std::move(answer));
        });
    }

private:
    void read() {
        // a fresh parser per request: beast's is not assignable and keeps state
        request_ = std::make_unique<http::request_parser<http::string_body>>();
        request_->body_limit(k_max_body_bytes);
        auto self = shared_from_this();
        http::async_read(stream_, buffer_, *request_, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                self->close();
                return;
            }
            self->dispatch();
        });
    }

    void dispatch();

    void write(Response answer) {
        response_ = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(answer.status),
                                                                        request_->get().version());
        response_->set(http::field::content_type, answer.content_type);
        // the terminal is served from another origin in development
        response_->set(http::field::access_control_allow_origin, "*");
        response_->result(static_cast<http::status>(answer.status));
        response_->reason(reason_of(answer.status));
        response_->body() = std::move(answer.body);
        response_->prepare_payload();
        response_->keep_alive(request_->get().keep_alive());

        auto self = shared_from_this();
        auto held = response_;
        http::async_write(stream_, *response_, [self, held](beast::error_code ec, std::size_t) {
            if (ec || !held->keep_alive()) {
                self->close();
                return;
            }
            self->read();
        });
    }

    void close() {
        beast::error_code ec;
        (void)stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::unique_ptr<http::request_parser<http::string_body>> request_;
    std::shared_ptr<http::response<http::string_body>> response_;
    const DeferredHandler* handler_;
};

} // namespace

struct Reply::Pending {
    explicit Pending(std::shared_ptr<Session> s) : session{std::move(s)} {}

    ~Pending() {
        if (!answered) {
            session->answer_later(failed());
        }
    }

    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;

    void answer(Response response) {
        const std::scoped_lock lock{mutex};
        if (answered) {
            return;
        }
        answered = true;
        if (in_handler) {
            held = std::move(response);
        } else {
            session->answer_later(std::move(response));
        }
    }

    std::optional<Response> close_window() {
        const std::scoped_lock lock{mutex};
        in_handler = false;
        return std::exchange(held, std::nullopt);
    }

    std::shared_ptr<Session> session;
    std::mutex mutex;
    bool in_handler{true};
    bool answered{false};
    std::optional<Response> held;
};

Reply::Reply(std::shared_ptr<Pending> pending) noexcept : pending_{std::move(pending)} {}

void Reply::operator()(Response response) const {
    pending_->answer(std::move(response));
}

namespace {

void Session::dispatch() {
    const auto target = std::string_view{request_->get().target()};
    const auto split = target.find('?');
    const auto& parsed = request_->get();
    const auto idempotency = parsed.find("Idempotency-Key");
    const auto login = parsed.find("X-Raikiri-Login");
    const Request req{
        .method = std::string_view{parsed.method_string()},
        .path = target.substr(0, split),
        .query = split == std::string_view::npos ? std::string_view{} : target.substr(split + 1),
        .body = std::string_view{parsed.body()},
        .idempotency_key = idempotency == parsed.end() ? std::string_view{} : std::string_view{idempotency->value()},
        .login = login == parsed.end() ? std::string_view{} : std::string_view{login->value()},
    };
    auto pending = std::make_shared<Reply::Pending>(shared_from_this());
    try {
        (*handler_)(req, Reply{pending});
    } catch (...) {
        Reply{pending}(failed());
    }
    // an answer given while the handler ran goes out now, as a plain handler's always did
    if (auto answer = pending->close_window()) {
        write(std::move(*answer));
    }
}

} // namespace

struct Server::Impl {
    Impl(void* shared, std::string addr, std::uint16_t p, DeferredHandler h)
        : owned{shared == nullptr ? std::make_unique<boost::asio::io_context>(1) : nullptr},
          ioc{shared == nullptr ? *owned : *static_cast<boost::asio::io_context*>(shared)}, address{std::move(addr)},
          port{p}, handler{std::move(h)}, acceptor{ioc} {}

    void accept() {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), handler)->run();
            }
            if (running) {
                accept();
            }
        });
    }

    std::unique_ptr<boost::asio::io_context> owned;
    boost::asio::io_context& ioc;
    std::string address;
    std::uint16_t port;
    DeferredHandler handler;
    tcp::acceptor acceptor;
    bool running{false};
};

namespace {

DeferredHandler deferred(Handler handler) {
    return [handler = std::move(handler)](const Request& request, const Reply& reply) { reply(handler(request)); };
}

} // namespace

Server::Server(std::string address, std::uint16_t port, Handler handler)
    : impl_{std::make_unique<Impl>(nullptr, std::move(address), port, deferred(std::move(handler)))} {}

Server::Server(net::IoContext& io, std::string address, std::uint16_t port, Handler handler)
    : impl_{std::make_unique<Impl>(io.native(), std::move(address), port, deferred(std::move(handler)))} {}

Server::Server(net::IoContext& io, std::string address, std::uint16_t port, DeferredHandler handler)
    : impl_{std::make_unique<Impl>(io.native(), std::move(address), port, std::move(handler))} {}

Server::~Server() = default;

bool Server::start() {
    beast::error_code ec;
    const auto addr = boost::asio::ip::make_address(impl_->address, ec);
    if (ec) {
        return false;
    }
    const tcp::endpoint endpoint{addr, impl_->port};
    (void)impl_->acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        return false;
    }
    (void)impl_->acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    (void)impl_->acceptor.bind(endpoint, ec);
    if (ec) {
        return false; // the port is taken: a configuration error, not a retry
    }
    (void)impl_->acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return false;
    }
    // port 0 lets tests bind any free port
    impl_->port = impl_->acceptor.local_endpoint(ec).port();
    impl_->running = true;
    impl_->accept();
    return true;
}

std::size_t Server::poll() noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    try {
        return impl_->ioc.poll();
    } catch (...) {
        return 0;
    }
}

std::size_t Server::run_for(std::chrono::nanoseconds timeout) noexcept {
    if (impl_->ioc.stopped()) {
        impl_->ioc.restart();
    }
    try {
        const auto ran = impl_->ioc.run_one_for(timeout);
        return ran + impl_->ioc.poll();
    } catch (...) {
        return 0;
    }
}

void Server::stop() noexcept {
    impl_->running = false;
    beast::error_code ec;
    (void)impl_->acceptor.close(ec);
    impl_->ioc.stop();
}

std::uint16_t Server::port() const noexcept {
    return impl_->port;
}

std::string_view query_param(std::string_view query, std::string_view key) noexcept {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::string percent_decoded(std::string_view text) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace raikiri::http
