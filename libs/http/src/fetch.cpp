#include <raikiri/http/server.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <string>

namespace raikiri::http {

namespace {

namespace beast = boost::beast;
namespace beast_http = beast::http;
using tcp = boost::asio::ip::tcp;

} // namespace

std::optional<Fetched> fetch(std::string_view host,
                             std::uint16_t port,
                             std::string_view target,
                             std::uint32_t timeout_ms,
                             std::string_view method,
                             std::string_view body) noexcept {
    try {
        boost::asio::io_context ioc;
        beast::tcp_stream stream{ioc};
        stream.expires_after(std::chrono::milliseconds{timeout_ms});

        tcp::resolver resolver{ioc};
        const auto endpoints = resolver.resolve(std::string{host}, std::to_string(port));
        stream.connect(endpoints);

        const auto verb = beast_http::string_to_verb(method);
        if (verb == beast_http::verb::unknown) {
            return std::nullopt;
        }
        beast_http::request<beast_http::string_body> req{verb, std::string{target}, 11};
        req.set(beast_http::field::host, std::string{host});
        if (!body.empty()) {
            req.set(beast_http::field::content_type, "application/json");
            req.body() = std::string{body};
        }
        req.prepare_payload();
        beast_http::write(stream, req);

        beast::flat_buffer buffer;
        beast_http::response<beast_http::string_body> res;
        beast_http::read(stream, buffer, res);

        beast::error_code ec;
        (void)stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        return Fetched{.status = static_cast<int>(res.result_int()), .body = std::move(res.body())};
    } catch (...) {
        // every failure maps to nullopt
        return std::nullopt;
    }
}

} // namespace raikiri::http
