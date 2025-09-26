#include <raikiri/venues/weex/endpoints.hpp>
#include <raikiri/venues/weex/rest.hpp>

namespace raikiri::venues::weex {

std::string_view rest_host() noexcept {
    return ep::k_rest_host;
}

net::RestRequest RequestBuilder::instruments() noexcept {
    return {.method = net::HttpMethod::Get, .target = ep::k_exchange_info};
}

net::RestRequest RequestBuilder::premium_index() noexcept {
    // NO `symbol`: the bare path answers for every contract in one body, which
    // is one call a minute rather than one per instrument watched.
    return {.method = net::HttpMethod::Get, .target = ep::k_premium_index};
}

} // namespace raikiri::venues::weex
