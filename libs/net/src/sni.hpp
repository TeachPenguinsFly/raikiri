#pragma once

// SSL_set_tlsext_host_name is a macro whose cast gcc's -Wold-style-cast blames on the caller
// without sni the handshake succeeds with the wrong certificate

#include <openssl/ssl.h>

namespace raikiri::net::internal {

[[nodiscard]] inline bool set_sni_host(::SSL* ssl, const char* host) noexcept {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    return SSL_set_tlsext_host_name(ssl, host) == 1;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

} // namespace raikiri::net::internal
