#pragma once

// Locating the CA trust store.

#include <string>
#include <string_view>

namespace raikiri::net {

struct TrustStore {
    std::string file; // CA bundle path, empty if none was found
    std::string dir;  // hashed CA directory, empty if none was found
    bool found{false};

    // Where it came from, for the startup log: an operator who sees
    // "SSL_CERT_FILE" knows their override took effect.
    std::string_view source;
};

// Resolution order:
//   1. $SSL_CERT_FILE / $SSL_CERT_DIR — the operator's explicit choice
//   2. OpenSSL's compiled-in default, if the path exists
//   3. Well-known bundles: /etc/ssl/cert.pem (macOS),
//      /etc/ssl/certs/ca-certificates.crt (Debian/Ubuntu),
//      /etc/pki/tls/certs/ca-bundle.crt (RHEL), Homebrew's copy
[[nodiscard]] TrustStore resolve_trust_store();

} // namespace raikiri::net
