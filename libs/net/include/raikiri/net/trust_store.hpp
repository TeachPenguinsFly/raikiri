#pragma once

// Locating the CA trust store.
//
// OpenSSL looks for certificates at a path baked in when it was compiled. For
// a package manager's build that path belongs to the build machine, so on a
// developer's laptop — and in a slim container — it simply does not exist,
// and every TLS handshake fails with "certificate verify failed". The error
// names the peer, which makes it read like the venue's problem rather than
// ours; the first live connection attempt to bybit failed exactly this way.
//
// So the store is resolved explicitly: the operator's override first, then
// the compiled-in default, then the handful of locations distributions and
// macOS actually use. Reporting which one was chosen matters as much as
// choosing it — silent fallback to an empty store is indistinguishable from
// working TLS right up until it is not.

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
