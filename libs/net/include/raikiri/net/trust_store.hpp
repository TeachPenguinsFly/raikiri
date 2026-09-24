#pragma once

// locating the ca trust store

#include <string>
#include <string_view>

namespace raikiri::net {

struct TrustStore {
    std::string file; // CA bundle path, empty if none was found
    std::string dir;  // hashed CA directory, empty if none was found
    bool found{false};

    std::string_view source;
};

// $SSL_CERT_FILE / $SSL_CERT_DIR, then openssl's default, then well-known bundles
[[nodiscard]] TrustStore resolve_trust_store();

} // namespace raikiri::net
