#pragma once

// Setting the SNI host on an OpenSSL connection, in the one place the cast can
// be contained.
//
// SSL_set_tlsext_host_name IS A MACRO that expands to SSL_ctrl with a
// `(char *)` inside it. gcc attributes that cast to whoever wrote the call, so
// -Wold-style-cast fails a build on a line containing no cast at all — and
// -Werror makes it fatal. Apple clang does not attribute it the same way,
// which is why two call sites sat here until this was first compiled on Linux.
//
// WITHOUT THE SNI THE HANDSHAKE STILL SUCCEEDS and the wrong certificate comes
// back, so this is not optional politeness: every venue here is behind a host
// that serves many names from one address.

#include <openssl/ssl.h>

namespace raikiri::net::detail {

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

} // namespace raikiri::net::detail
