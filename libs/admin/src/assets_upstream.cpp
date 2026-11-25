#include <raikiri/admin/assets.hpp>
#include <raikiri/net/rest_client.hpp>

#include <chrono>

namespace raikiri::admin {

namespace {

// a page waits on these, so a host that does not answer is given up on soon
constexpr std::uint32_t k_timeout_ms = 4'000;
constexpr std::uint32_t k_connect_timeout_ms = 2'500;
constexpr std::uint64_t k_document_limit = 4ULL << 20U;
constexpr std::uint64_t k_image_limit = 1ULL << 20U;
constexpr std::chrono::seconds k_idle{20};

net::RestConfig config(const AssetHost& host, std::uint64_t body_limit) {
    return {.host = host.host,
            .port = host.port,
            .pool_size = 1,
            .timeout_ms = k_timeout_ms,
            .connect_timeout_ms = k_connect_timeout_ms,
            .verify_tls = true,
            .body_limit = body_limit};
}

std::string escaped(std::string_view text) {
    constexpr std::string_view k_hex = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(k_hex[byte >> 4U]);
            out.push_back(k_hex[byte & 0x0FU]);
        }
    }
    return out;
}

struct Line {
    Line(net::IoContext& io, const AssetHost& host, std::uint64_t body_limit)
        : rest{io, config(host, body_limit)}, prefix{host.prefix} {}

    void ask(std::string_view path, std::string_view accept, AssetFetched done) {
        const auto now = std::chrono::steady_clock::now();
        // the far side reaps a silent socket without saying so; after a pause the next request opens its own
        if (rest.in_flight() == 0 && now - used > k_idle) {
            rest.stop();
        }
        used = now;
        const std::string target = prefix + std::string{path};
        const auto shared = std::make_shared<AssetFetched>(std::move(done));
        const bool sent = rest.send(
            {.method = net::HttpMethod::Get,
             .target = target,
             .body = {},
             .headers = {{.name = "Accept", .value = accept}},
             .content_type = {}},
            [shared](net::RestResponse&& response) { (*shared)(response.status, std::move(response.body)); });
        if (!sent) {
            (*shared)(0, {});
        }
    }

    net::RestClient rest;
    std::string prefix;
    std::chrono::steady_clock::time_point used{};
};

} // namespace

struct RestAssetUpstream::Impl {
    Impl(net::IoContext& io, const AssetHosts& hosts)
        : profiles{io, hosts.profiles, k_document_limit}, coin_images{io, hosts.coin_images, k_image_limit},
          stock_images{io, hosts.stock_images, k_image_limit} {}

    Line profiles;
    Line coin_images;
    Line stock_images;
};

RestAssetUpstream::RestAssetUpstream(net::IoContext& io, AssetHosts hosts) : impl_{std::make_unique<Impl>(io, hosts)} {}

RestAssetUpstream::~RestAssetUpstream() = default;

void RestAssetUpstream::coin(std::string_view id, AssetFetched done) {
    impl_->profiles.ask("/coins/" + escaped(id) +
                            "?localization=false&tickers=false&community_data=false&developer_data=false",
                        "application/json",
                        std::move(done));
}

void RestAssetUpstream::search(std::string_view symbol, AssetFetched done) {
    impl_->profiles.ask("/search?query=" + escaped(symbol), "application/json", std::move(done));
}

void RestAssetUpstream::image(AssetImageKind kind, std::string_view path, AssetFetched done) {
    auto& line = kind == AssetImageKind::Stock ? impl_->stock_images : impl_->coin_images;
    line.ask("/" + std::string{path}, "image/*", std::move(done));
}

} // namespace raikiri::admin
