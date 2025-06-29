#include <raikiri/admin/server.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <memory>
#include <utility>

namespace raikiri::admin {

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// Bounded so a malformed or hostile request cannot make the process allocate.
// Nothing this API accepts is large: every request is a GET with a short query.
constexpr std::uint64_t k_max_body_bytes = 16 * 1024;

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

// One connection, alive as long as its own handler chain holds it.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const Handler& handler)
        : stream_{std::move(socket)}, handler_{&handler} {}

    void run() { read(); }

private:
    void read() {
        // A fresh parser per request: beast's is not assignable, and reusing
        // one across requests on a keep-alive connection carries the previous
        // message's state into the next.
        request_ = std::make_unique<http::request_parser<http::string_body>>();
        request_->body_limit(k_max_body_bytes);
        auto self = shared_from_this();
        http::async_read(stream_, buffer_, *request_,
                         [self](beast::error_code ec, std::size_t) {
                             if (ec) {
                                 self->close();
                                 return;
                             }
                             self->dispatch();
                         });
    }

    void dispatch() {
        const auto target = std::string_view{request_->get().target()};
        const auto split = target.find('?');
        const Request req{.method = std::string_view{request_->get().method_string()},
                          .path = target.substr(0, split),
                          .query = split == std::string_view::npos ? std::string_view{}
                                                                   : target.substr(split + 1)};
        Response answer{};
        try {
            answer = (*handler_)(req);
        } catch (...) {
            // A handler that dies must not take the server with it, and the
            // client must not be left waiting on a socket that will never
            // answer.
            answer = {.status = 500,
                      .content_type = "application/problem+json",
                      .body = R"({"title":"handler failed","status":500})"};
        }
        write(std::move(answer));
    }

    void write(Response answer) {
        response_ = std::make_shared<http::response<http::string_body>>(
            static_cast<http::status>(answer.status), request_->get().version());
        response_->set(http::field::content_type, answer.content_type);
        // The terminal is served from somewhere else during development, so
        // the browser will not talk to this at all without it.
        response_->set(http::field::access_control_allow_origin, "*");
        response_->result(static_cast<http::status>(answer.status));
        response_->reason(reason_of(answer.status));
        response_->body() = std::move(answer.body);
        response_->prepare_payload();
        response_->keep_alive(request_->get().keep_alive());

        auto self = shared_from_this();
        auto held = response_;
        http::async_write(stream_, *response_,
                          [self, held](beast::error_code ec, std::size_t) {
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
    const Handler* handler_;
};

} // namespace

struct Server::Impl {
    Impl(std::string addr, std::uint16_t p, Handler h)
        : address{std::move(addr)}, port{p}, handler{std::move(h)}, acceptor{ioc} {}

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

    std::string address;
    std::uint16_t port;
    Handler handler;
    boost::asio::io_context ioc{1};
    tcp::acceptor acceptor;
    bool running{false};
};

Server::Server(std::string address, std::uint16_t port, Handler handler)
    : impl_{std::make_unique<Impl>(std::move(address), port, std::move(handler))} {}

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
    // Port 0 asks the kernel to choose, which is how a test binds without
    // agreeing a number with whatever else is running.
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

void Server::stop() noexcept {
    impl_->running = false;
    beast::error_code ec;
    (void)impl_->acceptor.close(ec);
    impl_->ioc.stop();
}

std::uint16_t Server::port() const noexcept { return impl_->port; }

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

} // namespace raikiri::admin
