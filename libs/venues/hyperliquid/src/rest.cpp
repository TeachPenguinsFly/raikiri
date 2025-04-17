#include <raikiri/venues/hyperliquid/rest.hpp>

namespace raikiri::venues::hyperliquid {

namespace {

constexpr std::string_view k_host = "api.hyperliquid.xyz";
constexpr std::string_view k_info = "/info";

} // namespace

std::string_view rest_host() noexcept {
    return k_host;
}

std::string_view info_path() noexcept {
    return k_info;
}

net::RestRequest RequestBuilder::meta() noexcept {
    return {.method = net::HttpMethod::Post, .target = k_info, .body = R"({"type":"meta"})"};
}

net::RestRequest RequestBuilder::meta_and_asset_ctxs() noexcept {
    return {.method = net::HttpMethod::Post,
            .target = k_info,
            .body = R"({"type":"metaAndAssetCtxs"})"};
}

} // namespace raikiri::venues::hyperliquid
