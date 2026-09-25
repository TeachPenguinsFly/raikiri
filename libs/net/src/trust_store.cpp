#include <raikiri/net/trust_store.hpp>

#include <openssl/x509.h>
#include <sys/stat.h>

#include <array>
#include <cstdlib>

namespace raikiri::net {

namespace {

bool exists(const char* path) noexcept {
    if (path == nullptr || *path == '\0') {
        return false;
    }

    struct stat st {};

    return ::stat(path, &st) == 0;
}

} // namespace

TrustStore resolve_trust_store() {
    TrustStore store{};

    if (const char* file = std::getenv(X509_get_default_cert_file_env()); exists(file)) {
        store.file = file;
        store.found = true;
        store.source = "SSL_CERT_FILE";
    }
    if (const char* dir = std::getenv(X509_get_default_cert_dir_env()); exists(dir)) {
        store.dir = dir;
        store.found = true;
        store.source = store.file.empty() ? "SSL_CERT_DIR" : "SSL_CERT_FILE+DIR";
    }
    if (store.found) {
        return store;
    }

    if (exists(X509_get_default_cert_file())) {
        store.file = X509_get_default_cert_file();
        store.found = true;
        store.source = "openssl-default";
        return store;
    }

    // 3. well-known bundle locations
    static constexpr std::array k_candidates{
        "/etc/ssl/cert.pem",                          // macOS
        "/etc/ssl/certs/ca-certificates.crt",         // Debian, Ubuntu, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",           // RHEL, Fedora
        "/etc/ssl/ca-bundle.pem",                     // SUSE
        "/opt/homebrew/etc/ca-certificates/cert.pem", // Homebrew on arm64
        "/usr/local/etc/ca-certificates/cert.pem",    // Homebrew on x86-64
    };
    for (const char* candidate : k_candidates) {
        if (exists(candidate)) {
            store.file = candidate;
            store.found = true;
            store.source = "system-bundle";
            return store;
        }
    }
    return store; // found == false: the caller must refuse to verify silently
}

} // namespace raikiri::net
