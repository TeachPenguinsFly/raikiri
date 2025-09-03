#pragma once

// Loopback TLS plumbing shared by the net client tests.
//
// The clients always negotiate TLS — the contract has no plaintext path — so
// the tests cannot fall back to a bare TCP server. Instead a throwaway
// self-signed certificate is minted in memory at test time and the client
// connects with verify_tls=false. Nothing touches the filesystem, nothing
// outlives the process, and no test needs a fixture checked into the repo.
//
// The listener runs its own io_context on a dedicated thread and every
// operation on it is asynchronous. A synchronous server would park that
// thread inside accept() with no portable way to wake it, which is precisely
// how test suites acquire hangs; io_context::stop() from another thread is
// always safe when nothing blocks inside a handler.

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace raikiri::net::test {

struct SelfSignedCert {
    std::string cert_pem;
    std::string key_pem;
};

namespace ossl {

struct PkeyDeleter {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};

struct CertDeleter {
    void operator()(X509* p) const noexcept { X509_free(p); }
};

struct BioDeleter {
    void operator()(BIO* p) const noexcept { BIO_free(p); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using CertPtr = std::unique_ptr<X509, CertDeleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

inline void require(bool ok, const char* what) {
    if (!ok) {
        throw std::runtime_error(std::string{"openssl: "} + what);
    }
}

inline std::string drain(BIO* bio) {
    std::string out;
    std::array<char, 1024> chunk{};
    int n = 0;
    while ((n = BIO_read(bio, chunk.data(), static_cast<int>(chunk.size()))) > 0) {
        out.append(chunk.data(), static_cast<std::size_t>(n));
    }
    return out;
}

} // namespace ossl

// P-256 rather than RSA: key generation is microseconds instead of a visible
// fraction of a second, and every test in the suite mints its own.
inline SelfSignedCert make_self_signed() {
    ossl::PkeyPtr key{EVP_EC_gen("prime256v1")};
    ossl::require(key != nullptr, "EVP_EC_gen");

    ossl::CertPtr cert{X509_new()};
    ossl::require(cert != nullptr, "X509_new");
    ossl::require(X509_set_version(cert.get(), 2) == 1, "X509_set_version");
    ossl::require(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1, "ASN1_INTEGER_set");
    ossl::require(X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) != nullptr, "notBefore");
    ossl::require(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) != nullptr, "notAfter");
    ossl::require(X509_set_pubkey(cert.get(), key.get()) == 1, "X509_set_pubkey");

    X509_NAME* name = X509_get_subject_name(cert.get());
    ossl::require(name != nullptr, "X509_get_subject_name");
    ossl::require(X509_NAME_add_entry_by_txt(name,
                                             "CN",
                                             MBSTRING_ASC,
                                             reinterpret_cast<const unsigned char*>("127.0.0.1"),
                                             -1,
                                             -1,
                                             0) == 1,
                  "X509_NAME_add_entry_by_txt");
    ossl::require(X509_set_issuer_name(cert.get(), name) == 1, "X509_set_issuer_name");
    ossl::require(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "X509_sign");

    ossl::BioPtr cert_bio{BIO_new(BIO_s_mem())};
    ossl::require(cert_bio != nullptr, "BIO_new");
    ossl::require(PEM_write_bio_X509(cert_bio.get(), cert.get()) == 1, "PEM_write_bio_X509");

    ossl::BioPtr key_bio{BIO_new(BIO_s_mem())};
    ossl::require(key_bio != nullptr, "BIO_new");
    ossl::require(PEM_write_bio_PrivateKey(
                      key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1,
                  "PEM_write_bio_PrivateKey");

    return SelfSignedCert{ossl::drain(cert_bio.get()), ossl::drain(key_bio.get())};
}

class TlsListener {
public:
    using Socket = boost::asio::ip::tcp::socket;
    using SslContext = boost::asio::ssl::context;
    // Handed the accepted socket rather than a finished TLS stream: the
    // websocket test wants to build websocket::stream<ssl_stream<tcp_stream>>
    // around it and drive the handshake itself, and the HTTP test does not.
    using OnSession = std::function<void(Socket&&, SslContext&)>;

    explicit TlsListener(OnSession on_session)
        : on_session_{std::move(on_session)}, tls_{SslContext::tls_server}, acceptor_{ioc_} {
        const auto cert = make_self_signed();
        tls_.use_certificate_chain(boost::asio::buffer(cert.cert_pem));
        tls_.use_private_key(boost::asio::buffer(cert.key_pem), SslContext::file_format::pem);

        const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"),
                                                      0};
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::socket_base::reuse_address{true});
        acceptor_.bind(endpoint);
        acceptor_.listen(boost::asio::socket_base::max_listen_connections);
        port_ = acceptor_.local_endpoint().port();

        do_accept();
        thread_ = std::thread{[this] { ioc_.run(); }};
    }

    TlsListener(const TlsListener&) = delete;
    TlsListener& operator=(const TlsListener&) = delete;

    ~TlsListener() { stop(); }

    void stop() noexcept {
        if (stopped_.exchange(true)) {
            return;
        }
        ioc_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::size_t accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](const boost::system::error_code& ec, Socket socket) {
            if (ec) {
                return;
            }
            accepted_.fetch_add(1, std::memory_order_relaxed);
            on_session_(std::move(socket), tls_);
            do_accept();
        });
    }

    OnSession on_session_;
    boost::asio::io_context ioc_{1};
    SslContext tls_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_{0};
    std::atomic<std::size_t> accepted_{0};
    std::atomic<bool> stopped_{false};
    std::thread thread_;
};

// Binds and immediately releases an ephemeral port. Nothing is listening on
// the returned number, which is what the transport-failure test needs.
inline std::uint16_t reserve_closed_port() {
    boost::asio::io_context probe{1};
    boost::asio::ip::tcp::acceptor acceptor{
        probe, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    return acceptor.local_endpoint().port();
}

} // namespace raikiri::net::test
