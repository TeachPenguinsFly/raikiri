#include <raikiri/sdk/client.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>

namespace raikiri::sdk {

std::string sign_request(std::string_view secret,
                         std::string_view timestamp_ms,
                         std::string_view method,
                         std::string_view path,
                         std::string_view query,
                         std::string_view body) {
    std::string message;
    message.reserve(timestamp_ms.size() + method.size() + path.size() + query.size() + body.size() + 4);
    message.append(timestamp_ms).append(1, '\n');
    message.append(method).append(1, '\n');
    message.append(path).append(1, '\n');
    message.append(query).append(1, '\n');
    message.append(body);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    ::HMAC(::EVP_sha256(),
           secret.data(),
           static_cast<int>(secret.size()),
           reinterpret_cast<const unsigned char*>(message.data()),
           message.size(),
           digest.data(),
           &size);
    static constexpr std::string_view k_digits = "0123456789abcdef";
    std::string out;
    out.reserve(std::size_t{size} * 2);
    for (unsigned int i = 0; i < size; ++i) {
        out.push_back(k_digits[digest[i] >> 4U]);
        out.push_back(k_digits[digest[i] & 0xFU]);
    }
    return out;
}

} // namespace raikiri::sdk
