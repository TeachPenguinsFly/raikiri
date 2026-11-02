#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <charconv>
#include <memory>

#include "internal.hpp"
#include "sni.hpp"

namespace raikiri::sdk::internal {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

} // namespace

std::optional<Url> parse_url(std::string_view url) {
    Url out{};
    if (url.starts_with("https://")) {
        out.tls = true;
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        url.remove_prefix(7);
    } else {
        return std::nullopt;
    }
    while (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    if (url.empty() || url.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = url.rfind(':');
    out.host = std::string{url.substr(0, colon)};
    out.port = colon == std::string_view::npos ? (out.tls ? "443" : "80") : std::string{url.substr(colon + 1)};
    return out;
}

std::string percent_encode(std::string_view text) {
    static constexpr std::string_view k_digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(k_digits[byte >> 4U]);
            out.push_back(k_digits[byte & 0xFU]);
        }
    }
    return out;
}

struct Transport::Impl {
    Url url;
    std::chrono::milliseconds timeout;
    asio::io_context io;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    std::unique_ptr<beast::tcp_stream> plain;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> secure;
    beast::flat_buffer buffer;

    Impl(Url u, std::chrono::milliseconds t) : url{std::move(u)}, timeout{t} {
        if (url.tls) {
            tls.set_default_verify_paths();
            tls.set_verify_mode(asio::ssl::verify_peer);
        }
    }

    void disconnect() {
        plain.reset();
        secure.reset();
        buffer.clear();
    }

    void connect() {
        tcp::resolver resolver{io};
        const auto endpoints = resolver.resolve(url.host, url.port);
        if (url.tls) {
            secure = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(io, tls);
            if (!set_sni_host(secure->native_handle(), url.host.c_str())) {
                throw beast::system_error{
                    beast::error_code{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()}};
            }
            secure->set_verify_callback(asio::ssl::host_name_verification{url.host});
            beast::get_lowest_layer(*secure).expires_after(timeout);
            beast::get_lowest_layer(*secure).connect(endpoints);
            secure->handshake(asio::ssl::stream_base::client);
        } else {
            plain = std::make_unique<beast::tcp_stream>(io);
            plain->expires_after(timeout);
            plain->connect(endpoints);
        }
    }

    template <typename Next> HttpReply round_trip(Next& next, http::request<http::string_body>& request) {
        http::write(next, request);
        http::response_parser<http::string_body> parser;
        parser.body_limit(std::uint64_t{64} * 1024 * 1024);
        http::read(next, buffer, parser);
        auto& message = parser.get();
        HttpReply reply{.status = static_cast<int>(message.result_int()), .body = std::move(message.body())};
        if (const auto id = message.find("X-Request-Id"); id != message.end()) {
            reply.request_id = std::string{id->value()};
        }
        if (const auto after = message.find(http::field::retry_after); after != message.end()) {
            std::int64_t seconds = 0;
            const auto text = std::string_view{after->value()};
            if (std::from_chars(text.data(), text.data() + text.size(), seconds).ec == std::errc{}) {
                reply.retry_after = std::chrono::seconds{seconds};
            }
        }
        if (const auto left = message.find("RateLimit-Remaining"); left != message.end()) {
            std::int64_t remaining = 0;
            const auto text = std::string_view{left->value()};
            if (std::from_chars(text.data(), text.data() + text.size(), remaining).ec == std::errc{}) {
                reply.rate_limit_remaining = remaining;
            }
        }
        reply.replayed = message.find("Idempotent-Replay") != message.end();
        if (!message.keep_alive()) {
            disconnect();
        }
        return reply;
    }

    HttpReply once(http::request<http::string_body>& request) {
        if (plain == nullptr && secure == nullptr) {
            connect();
        }
        if (url.tls) {
            auto& tls_stream = *secure;
            beast::get_lowest_layer(tls_stream).expires_after(timeout);
            return round_trip(tls_stream, request);
        }
        auto& plain_stream = *plain;
        plain_stream.expires_after(timeout);
        return round_trip(plain_stream, request);
    }
};

Transport::Transport(Url url, std::chrono::milliseconds timeout)
    : impl_{std::make_unique<Impl>(std::move(url), timeout)} {}

Transport::~Transport() = default;

std::expected<HttpReply, std::string> Transport::exchange(std::string_view method,
                                                          std::string_view target,
                                                          const std::vector<Header>& headers,
                                                          std::string_view body) {
    http::request<http::string_body> request{http::string_to_verb(method), std::string{target}, 11};
    request.set(http::field::host, impl_->url.host);
    request.keep_alive(true);
    for (const auto& header : headers) {
        request.set(header.name, header.value);
    }
    if (!body.empty()) {
        request.set(http::field::content_type, "application/json");
        request.body() = std::string{body};
    }
    request.prepare_payload();
    try {
        return impl_->once(request);
    } catch (const std::exception& error) {
        impl_->disconnect();
        return std::unexpected(std::string{error.what()});
    }
}

} // namespace raikiri::sdk::internal
