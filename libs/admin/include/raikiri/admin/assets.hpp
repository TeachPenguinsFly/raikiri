#pragma once

// what a coin is and what its mark looks like, asked of a public catalogue on the browser's behalf: the terminal
// calls nothing but admind, so the catalogue's answer is reshaped here and its images are kept on disk

#include <raikiri/common/time.hpp>
#include <raikiri/http/server.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace raikiri::net {
class IoContext;
}

namespace raikiri::admin {

// http and https addresses only
struct AssetLinks {
    std::optional<std::string> website{};
    std::optional<std::string> whitepaper{};
    std::optional<std::string> explorer{};
};

// numbers are decimal text and absent where the catalogue has none; times are as the catalogue writes them
struct AssetProfile {
    std::string symbol;
    std::string name;
    std::optional<std::uint32_t> rank{};
    std::optional<std::string> market_cap{};
    std::optional<std::string> fully_diluted{};
    std::optional<std::string> circulating_supply{};
    std::optional<std::string> total_supply{};
    std::optional<std::string> max_supply{};
    std::optional<std::string> ath{};
    std::optional<std::string> ath_at{};
    std::optional<std::string> atl{};
    std::optional<std::string> atl_at{};
    std::optional<std::string> launched{};
    AssetLinks links{};
    std::vector<std::string> categories{};
    std::string description;
};

inline constexpr std::size_t k_asset_symbol_max = 24;
inline constexpr std::size_t k_asset_description_max = 4'000;
inline constexpr std::size_t k_asset_icon_max = 512U << 10U;

// letters, digits, '.', '_' and '-', not led by a dot: safe as a file name and as a path segment
[[nodiscard]] bool valid_asset_symbol(std::string_view symbol) noexcept;

// venues list small coins by the thousand or the million: 1000PEPE and kPEPE are PEPE
[[nodiscard]] std::string_view strip_multiplier(std::string_view symbol) noexcept;

// no exponent and no trailing zeros; empty for what is not a finite positive number
[[nodiscard]] std::string asset_decimal(double value);

// the catalogue's coin document as ours; nullopt when it is not one
[[nodiscard]] std::optional<AssetProfile> parse_asset_profile(std::string_view json, std::string_view symbol);

// the catalogue's search answer: the largest coin carrying exactly this symbol, empty when none does and nullopt
// when the answer cannot be read
[[nodiscard]] std::optional<std::string> pick_asset_id(std::string_view json, std::string_view symbol);

// png, jpeg, gif or webp by its leading bytes, empty otherwise; svg is refused, it can carry script
[[nodiscard]] std::string_view image_type(std::string_view bytes) noexcept;

// where a symbol's image is kept under `dir`; nullopt for a symbol that is not a plain file name
[[nodiscard]] std::optional<std::filesystem::path> icon_path(const std::filesystem::path& dir, std::string_view symbol);

struct AssetMark {
    std::string id;    // the catalogue's coin id, empty when unmatched or a stock
    std::string image; // path under the image host, empty when the terminal bundles the mark
    bool stock{false};
};

// scripts/coin_icons.py's marks.json, the file the terminal itself reads: ticker -> [id, name, image, "eq"?]
class AssetMarks {
public:
    [[nodiscard]] static std::optional<AssetMarks> parse(std::string_view json);
    [[nodiscard]] static std::optional<AssetMarks> load(const std::string& path);

    struct Found {
        std::string key; // the ticker as the catalogue keys it, upper case
        const AssetMark* mark{nullptr};
    };

    // the ticker as written first, then without its multiplier
    [[nodiscard]] Found find(std::string_view symbol) const;

    [[nodiscard]] std::size_t size() const noexcept { return marks_.size(); }

private:
    std::unordered_map<std::string, AssetMark> marks_;
};

enum class AssetImageKind : std::uint8_t { Coin, Stock };

// status 0 is no answer at all
using AssetFetched = std::function<void(int status, std::string body)>;

// asked without waiting, like the margin gateway: the answer arrives from the loop, or at once when nothing was sent
class AssetUpstream {
public:
    AssetUpstream() = default;
    AssetUpstream(const AssetUpstream&) = delete;
    AssetUpstream& operator=(const AssetUpstream&) = delete;
    virtual ~AssetUpstream() = default;

    virtual void coin(std::string_view id, AssetFetched done) = 0;
    virtual void search(std::string_view symbol, AssetFetched done) = 0;
    virtual void image(AssetImageKind kind, std::string_view path, AssetFetched done) = 0;
};

// https only; `prefix` is put before every path asked
struct AssetHost {
    std::string host;
    std::string port{"443"};
    std::string prefix{};

    // "https://host[:port][/prefix]"; nullopt for anything else
    [[nodiscard]] static std::optional<AssetHost> parse(std::string_view url);
};

struct AssetHosts {
    AssetHost profiles{.host = "api.coingecko.com", .prefix = "/api/v3"};
    AssetHost coin_images{.host = "coin-images.coingecko.com", .prefix = "/coins/images"};
    AssetHost stock_images{.host = "financialmodelingprep.com", .prefix = "/image-stock"};
};

// one connection per host, opened when first asked and dropped once idle; no cookie is kept
class RestAssetUpstream final : public AssetUpstream {
public:
    RestAssetUpstream(net::IoContext& io, AssetHosts hosts);
    ~RestAssetUpstream() override;

    void coin(std::string_view id, AssetFetched done) override;
    void search(std::string_view symbol, AssetFetched done) override;
    void image(AssetImageKind kind, std::string_view path, AssetFetched done) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// images by symbol in one directory, written whole and renamed into place
class IconStore {
public:
    explicit IconStore(std::filesystem::path dir) : dir_{std::move(dir)} {}

    [[nodiscard]] std::optional<std::string> read(std::string_view symbol) const;
    [[nodiscard]] bool write(std::string_view symbol, std::string_view bytes) const;

private:
    std::filesystem::path dir_;
};

struct AssetError {
    int status{500};
    std::string title;
    std::string detail;
};

struct AssetIcon {
    std::string content_type;
    std::string bytes;
};

using AssetProfileDone = std::function<void(std::expected<AssetProfile, AssetError>)>;
using AssetIconDone = std::function<void(std::expected<AssetIcon, AssetError>)>;

struct AssetLimits {
    std::uint64_t fresh_ns{600ULL * 1'000'000'000ULL};
    std::size_t profiles{512};
    // an image the host does not have is not asked for again for an hour, one it failed to send for half a minute
    std::uint64_t missing_ns{3'600ULL * 1'000'000'000ULL};
    std::uint64_t failed_ns{30ULL * 1'000'000'000ULL};
};

// `done` runs when the upstream answers, or at once from memory, disk or a refusal
class AssetDesk {
public:
    // null marks resolve every coin by search and serve no image; a null store serves no image
    AssetDesk(AssetUpstream& upstream,
              const AssetMarks* marks,
              const IconStore* icons,
              std::function<MonoNanos()> now,
              AssetLimits limits = {});

    void profile(std::string_view symbol, AssetProfileDone done);
    void icon(std::string_view symbol, AssetIconDone done);

    [[nodiscard]] std::size_t cached() const noexcept { return profiles_.size(); }

private:
    struct Kept {
        std::optional<AssetProfile> profile; // empty: the catalogue has no such coin
        std::uint64_t fresh_until_ns{0};
    };

    [[nodiscard]] AssetMarks::Found find(std::string_view symbol) const;
    void ask_coin(const std::string& key, const std::string& id);
    void keep(const std::string& key, std::optional<AssetProfile> profile);
    void finish(const std::string& key);
    void finish_icon(const std::string& key, std::expected<AssetIcon, AssetError> result);

    AssetUpstream* upstream_;
    const AssetMarks* marks_;
    const IconStore* icons_;
    std::function<MonoNanos()> now_;
    AssetLimits limits_;
    std::unordered_map<std::string, Kept> profiles_;
    std::unordered_map<std::string, std::vector<AssetProfileDone>> asking_;
    std::unordered_map<std::string, std::vector<AssetIconDone>> fetching_;
    std::unordered_map<std::string, std::uint64_t> no_icon_until_;
};

using AssetRespond = std::function<void(http::Response)>;

// GET /v1/assets/{symbol} and /v1/assets/{symbol}/icon; false for any other path, which leaves `respond` unused
class AssetRoutes {
public:
    explicit AssetRoutes(AssetDesk& desk) : desk_{&desk} {}

    [[nodiscard]] bool operator()(const http::Request& req, const AssetRespond& respond) const;

private:
    AssetDesk* desk_;
};

} // namespace raikiri::admin
