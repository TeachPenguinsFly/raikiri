#include <raikiri/venues/htx/rest.hpp>

namespace raikiri::venues::htx {

namespace {

constexpr std::string_view k_host = "api.hbdm.com";

} // namespace

std::string_view rest_host() noexcept {
    return k_host;
}

net::RestRequest RequestBuilder::contracts() noexcept {
    return {.method = net::HttpMethod::Get,
            .target = "/linear-swap-api/v1/swap_contract_info",
            .body = {}};
}

net::RestRequest RequestBuilder::server_time() noexcept {
    return {.method = net::HttpMethod::Get, .target = "/api/v1/timestamp", .body = {}};
}

} // namespace raikiri::venues::htx
