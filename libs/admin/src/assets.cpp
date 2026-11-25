#include <raikiri/admin/assets.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>
#include <system_error>

namespace raikiri::admin {

struct CoinUsd {
    std::optional<double> usd{};
};

struct CoinUsdDate {
    std::optional<std::string> usd{};
};

struct CoinDescription {
    std::optional<std::string> en{};
};

using CoinTexts = std::optional<std::vector<std::optional<std::string>>>;

struct CoinLinks {
    CoinTexts homepage{};
    std::optional<std::string> whitepaper{};
    CoinTexts blockchain_site{};
};

struct CoinMarket {
    std::optional<CoinUsd> market_cap{};
    std::optional<CoinUsd> fully_diluted_valuation{};
    std::optional<double> circulating_supply{};
    std::optional<double> total_supply{};
    std::optional<double> max_supply{};
    std::optional<CoinUsd> ath{};
    std::optional<CoinUsdDate> ath_date{};
    std::optional<CoinUsd> atl{};
    std::optional<CoinUsdDate> atl_date{};
};

struct CoinDoc {
    std::optional<std::string> id{};
    std::optional<std::string> name{};
    std::optional<double> market_cap_rank{};
    std::optional<std::string> genesis_date{};
    std::optional<CoinDescription> description{};
    std::optional<CoinLinks> links{};
    CoinTexts categories{};
    std::optional<CoinMarket> market_data{};
};

struct SearchCoin {
    std::optional<std::string> id{};
    std::optional<std::string> symbol{};
    std::optional<double> market_cap_rank{};
};

struct SearchDoc {
    std::optional<std::vector<SearchCoin>> coins{};
};

namespace {

constexpr glz::opts k_lenient{.error_on_unknown_keys = false};
constexpr glz::opts k_opts{.skip_null_members = true};
constexpr std::string_view k_prefix = "/v1/assets/";
constexpr std::string_view k_icon_suffix = "/icon";
constexpr std::string_view k_icon_cache = "private, max-age=604800";
constexpr std::size_t k_time_max = 40;
constexpr std::size_t k_link_max = 300;
constexpr std::size_t k_categories_max = 16;

bool is_upper(char c) noexcept {
    return c >= 'A' && c <= 'Z';
}

bool is_alnum(char c) noexcept {
    return is_upper(c) || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

char upper(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

std::string upper(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) { return upper(c); });
    return out;
}

bool same_symbol(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return upper(x) == upper(y); });
}

std::optional<std::string> amount(const std::optional<double>& value) {
    auto text = value.has_value() ? asset_decimal(*value) : std::string{};
    return text.empty() ? std::nullopt : std::optional<std::string>{std::move(text)};
}

std::optional<std::string> amount(const std::optional<CoinUsd>& value) {
    return value.has_value() ? amount(value->usd) : std::nullopt;
}

std::optional<std::string> time_text(const std::optional<std::string>& value) {
    if (!value.has_value() || value->empty() || value->size() > k_time_max) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> time_text(const std::optional<CoinUsdDate>& value) {
    return value.has_value() ? time_text(value->usd) : std::nullopt;
}

bool web_address(std::string_view text) noexcept {
    return (text.starts_with("https://") || text.starts_with("http://")) && text.size() <= k_link_max &&
           std::ranges::all_of(text, [](char c) { return c > 0x20 && c < 0x7f && c != '"' && c != '<'; });
}

std::optional<std::string> first_address(const CoinTexts& texts) {
    if (texts.has_value()) {
        for (const auto& text : *texts) {
            if (text.has_value() && web_address(*text)) {
                return text;
            }
        }
    }
    return std::nullopt;
}

// the catalogue writes descriptions with anchors in them; the terminal shows text
std::string plain_text(std::string_view html) {
    std::string out;
    out.reserve(std::min(html.size(), k_asset_description_max));
    for (std::size_t i = 0; i < html.size(); ++i) {
        if (html[i] == '<') {
            if (const auto close = html.find('>', i + 1); close != std::string_view::npos && close > i + 1) {
                i = close;
                continue;
            }
        }
        out.push_back(html[i]);
    }
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    out.erase(out.find_last_not_of(" \t\r\n") + 1);
    out.erase(0, first);
    if (out.size() > k_asset_description_max) {
        auto cut = k_asset_description_max;
        // not through the middle of a utf-8 sequence
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        out.resize(cut);
    }
    return out;
}

bool image_path(std::string_view path) noexcept {
    return !path.empty() && path.size() <= k_link_max && path.front() != '/' &&
           path.find("..") == std::string_view::npos &&
           std::ranges::all_of(path, [](char c) { return c > 0x20 && c < 0x7f && c != '?' && c != '#' && c != '\\'; });
}

std::expected<AssetProfile, AssetError> no_coin(std::string_view symbol) {
    return std::unexpected{AssetError{404, "no such asset", "no coin is known as " + std::string{symbol}}};
}

http::Response refusal(const AssetError& error) {
    return http::problem(error.status, error.title, error.detail);
}

} // namespace

bool valid_asset_symbol(std::string_view symbol) noexcept {
    return !symbol.empty() && symbol.size() <= k_asset_symbol_max && symbol.front() != '.' &&
           std::ranges::all_of(symbol, [](char c) { return is_alnum(c) || c == '.' || c == '_' || c == '-'; });
}

std::string_view strip_multiplier(std::string_view symbol) noexcept {
    for (const std::string_view prefix : {"1000000", "10000", "1000", "1M", "k"}) {
        if (symbol.starts_with(prefix) && symbol.size() > prefix.size() && is_upper(symbol[prefix.size()])) {
            return symbol.substr(prefix.size());
        }
    }
    return symbol;
}

std::string asset_decimal(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return {};
    }
    // the largest double written out has 309 digits
    std::array<char, 400> text{};
    const auto [end, ec] = std::to_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
    return ec == std::errc{} ? std::string{text.data(), end} : std::string{};
}

std::optional<AssetProfile> parse_asset_profile(std::string_view json, std::string_view symbol) {
    CoinDoc doc{};
    if (const auto ec = glz::read<k_lenient>(doc, json); ec) {
        return std::nullopt;
    }
    if (!doc.id.has_value() || doc.id->empty()) {
        return std::nullopt;
    }
    AssetProfile out{.symbol = std::string{symbol}, .name = doc.name.value_or(std::string{symbol}), .description = {}};
    if (const auto rank = doc.market_cap_rank.value_or(0.0); std::isfinite(rank) && rank >= 1.0 && rank < 1e9) {
        out.rank = static_cast<std::uint32_t>(rank);
    }
    if (doc.market_data.has_value()) {
        const auto& market = *doc.market_data;
        out.market_cap = amount(market.market_cap);
        out.fully_diluted = amount(market.fully_diluted_valuation);
        out.circulating_supply = amount(market.circulating_supply);
        out.total_supply = amount(market.total_supply);
        out.max_supply = amount(market.max_supply);
        out.ath = amount(market.ath);
        out.ath_at = time_text(market.ath_date);
        out.atl = amount(market.atl);
        out.atl_at = time_text(market.atl_date);
    }
    out.launched = time_text(doc.genesis_date);
    if (doc.links.has_value()) {
        out.links.website = first_address(doc.links->homepage);
        if (doc.links->whitepaper.has_value() && web_address(*doc.links->whitepaper)) {
            out.links.whitepaper = doc.links->whitepaper;
        }
        out.links.explorer = first_address(doc.links->blockchain_site);
    }
    if (doc.categories.has_value()) {
        for (const auto& category : *doc.categories) {
            if (category.has_value() && !category->empty() && out.categories.size() < k_categories_max) {
                out.categories.push_back(*category);
            }
        }
    }
    if (doc.description.has_value() && doc.description->en.has_value()) {
        out.description = plain_text(*doc.description->en);
    }
    return out;
}

std::optional<std::string> pick_asset_id(std::string_view json, std::string_view symbol) {
    SearchDoc doc{};
    if (const auto ec = glz::read<k_lenient>(doc, json); ec) {
        return std::nullopt;
    }
    if (!doc.coins.has_value()) {
        return std::nullopt;
    }
    std::string best;
    double best_rank = std::numeric_limits<double>::infinity();
    for (const auto& coin : *doc.coins) {
        if (!coin.id.has_value() || coin.id->empty() || !coin.symbol.has_value() ||
            !same_symbol(*coin.symbol, symbol)) {
            continue;
        }
        // unranked coins come after ranked ones, in the order the catalogue gave
        const auto rank = coin.market_cap_rank.value_or(std::numeric_limits<double>::infinity());
        if (best.empty() || rank < best_rank) {
            best = *coin.id;
            best_rank = rank;
        }
    }
    return best;
}

std::string_view image_type(std::string_view bytes) noexcept {
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) {
        return "image/png";
    }
    if (bytes.starts_with("\xff\xd8\xff")) {
        return "image/jpeg";
    }
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) {
        return "image/gif";
    }
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") {
        return "image/webp";
    }
    return {};
}

std::optional<std::filesystem::path> icon_path(const std::filesystem::path& dir, std::string_view symbol) {
    if (!valid_asset_symbol(symbol)) {
        return std::nullopt;
    }
    return dir / std::string{symbol};
}

std::optional<AssetMarks> AssetMarks::parse(std::string_view json) {
    std::unordered_map<std::string, std::vector<std::string>> rows;
    if (const auto ec = glz::read<k_lenient>(rows, json); ec) {
        return std::nullopt;
    }
    AssetMarks out;
    for (auto& [ticker, row] : rows) {
        if (!valid_asset_symbol(ticker) || row.size() < 3) {
            continue;
        }
        AssetMark mark{.id = std::move(row[0]), .image = std::move(row[2]), .stock = row.size() > 3 && row[3] == "eq"};
        if (!image_path(mark.image)) {
            mark.image.clear();
        }
        out.marks_.emplace(upper(ticker), std::move(mark));
    }
    return out;
}

std::optional<AssetMarks> AssetMarks::load(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return parse(text);
}

AssetMarks::Found AssetMarks::find(std::string_view symbol) const {
    auto key = upper(symbol);
    if (const auto it = marks_.find(key); it != marks_.end()) {
        return {.key = std::move(key), .mark = &it->second};
    }
    key = upper(strip_multiplier(symbol));
    const auto it = marks_.find(key);
    return {.key = std::move(key), .mark = it == marks_.end() ? nullptr : &it->second};
}

std::optional<AssetHost> AssetHost::parse(std::string_view url) {
    constexpr std::string_view k_scheme = "https://";
    if (!url.starts_with(k_scheme)) {
        return std::nullopt;
    }
    url.remove_prefix(k_scheme.size());
    const auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    auto prefix = slash == std::string_view::npos ? std::string_view{} : url.substr(slash);
    while (prefix.ends_with('/')) {
        prefix.remove_suffix(1);
    }
    AssetHost out{.host = {}, .port = "443", .prefix = std::string{prefix}};
    if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
        out.port = std::string{authority.substr(colon + 1)};
        authority = authority.substr(0, colon);
    }
    out.host = std::string{authority};
    const auto host_char = [](char c) { return is_alnum(c) || c == '.' || c == '-'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (out.host.empty() || !std::ranges::all_of(out.host, host_char) || out.port.empty() || out.port.size() > 5 ||
        !std::ranges::all_of(out.port, digit) ||
        !std::ranges::all_of(out.prefix, [](char c) { return c > 0x20 && c < 0x7f && c != '?' && c != '#'; })) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> IconStore::read(std::string_view symbol) const {
    const auto path = icon_path(dir_, symbol);
    if (!path.has_value()) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(*path, ec);
    if (ec || size == 0 || size > k_asset_icon_max) {
        return std::nullopt;
    }
    std::ifstream in{*path, std::ios::binary};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}

bool IconStore::write(std::string_view symbol, std::string_view bytes) const {
    const auto path = icon_path(dir_, symbol);
    if (!path.has_value() || bytes.empty() || bytes.size() > k_asset_icon_max) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // no symbol starts with a dot, so a half-written file is never read as one
    const auto part = dir_ / ("." + std::string{symbol} + ".part");
    {
        std::ofstream out{part, std::ios::binary | std::ios::trunc};
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
            std::filesystem::remove(part, ec);
            return false;
        }
    }
    std::filesystem::rename(part, *path, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

AssetDesk::AssetDesk(AssetUpstream& upstream,
                     const AssetMarks* marks,
                     const IconStore* icons,
                     std::function<MonoNanos()> now,
                     AssetLimits limits)
    : upstream_{&upstream}, marks_{marks}, icons_{icons}, now_{std::move(now)}, limits_{limits} {}

AssetMarks::Found AssetDesk::find(std::string_view symbol) const {
    if (marks_ != nullptr) {
        return marks_->find(symbol);
    }
    return {.key = upper(strip_multiplier(symbol)), .mark = nullptr};
}

void AssetDesk::profile(std::string_view symbol, AssetProfileDone done) {
    if (!valid_asset_symbol(symbol)) {
        done(std::unexpected{AssetError{400, "bad symbol", "letters, digits, '.', '_' and '-', 24 at most"}});
        return;
    }
    const auto found = find(symbol);
    if (found.mark != nullptr && found.mark->stock) {
        done(no_coin(found.key));
        return;
    }
    const auto& key = found.key;
    if (const auto it = profiles_.find(key); it != profiles_.end() && now_().ns < it->second.fresh_until_ns) {
        done(it->second.profile.has_value() ? std::expected<AssetProfile, AssetError>{*it->second.profile}
                                            : no_coin(key));
        return;
    }
    // one question per coin however many ask at once
    auto& waiting = asking_[key];
    waiting.push_back(std::move(done));
    if (waiting.size() > 1) {
        return;
    }
    if (found.mark != nullptr && !found.mark->id.empty()) {
        ask_coin(key, found.mark->id);
        return;
    }
    upstream_->search(key, [this, key](int status, std::string body) {
        const auto id = status == 200 ? pick_asset_id(body, key) : std::nullopt;
        if (!id.has_value()) {
            finish(key);
        } else if (id->empty()) {
            keep(key, std::nullopt);
            finish(key);
        } else {
            ask_coin(key, *id);
        }
    });
}

void AssetDesk::ask_coin(const std::string& key, const std::string& id) {
    upstream_->coin(id, [this, key](int status, std::string body) {
        if (status == 404) {
            keep(key, std::nullopt);
        } else if (status == 200) {
            if (auto profile = parse_asset_profile(body, key); profile.has_value()) {
                keep(key, std::move(profile));
            }
        }
        finish(key);
    });
}

void AssetDesk::keep(const std::string& key, std::optional<AssetProfile> profile) {
    if (!profiles_.contains(key) && profiles_.size() >= limits_.profiles) {
        const auto oldest = std::ranges::min_element(
            profiles_, {}, [](const auto& entry) { return entry.second.fresh_until_ns; });
        profiles_.erase(oldest);
    }
    profiles_.insert_or_assign(key,
                               Kept{.profile = std::move(profile), .fresh_until_ns = now_().ns + limits_.fresh_ns});
}

void AssetDesk::finish(const std::string& key) {
    auto node = asking_.extract(key);
    if (node.empty()) {
        return;
    }
    std::expected<AssetProfile, AssetError> result{
        std::unexpected{AssetError{503, "asset catalogue is not answering", "worth retrying"}}};
    if (const auto it = profiles_.find(key); it != profiles_.end()) {
        auto& kept = it->second;
        // what is kept answers a failed ask however old it is, and is not asked about again for a while
        if (const auto now = now_().ns; now >= kept.fresh_until_ns) {
            kept.fresh_until_ns = now + limits_.failed_ns;
        }
        result = kept.profile.has_value() ? std::expected<AssetProfile, AssetError>{*kept.profile} : no_coin(key);
    }
    for (const auto& done : node.mapped()) {
        done(result);
    }
}

void AssetDesk::icon(std::string_view symbol, AssetIconDone done) {
    if (!valid_asset_symbol(symbol)) {
        done(std::unexpected{AssetError{400, "bad symbol", "letters, digits, '.', '_' and '-', 24 at most"}});
        return;
    }
    const AssetError none{404, "no image for this asset", {}};
    if (icons_ == nullptr || marks_ == nullptr) {
        done(std::unexpected{none});
        return;
    }
    const auto found = marks_->find(symbol);
    if (found.mark == nullptr || found.mark->image.empty()) {
        done(std::unexpected{none});
        return;
    }
    const auto& key = found.key;
    if (auto bytes = icons_->read(key); bytes.has_value()) {
        if (const auto type = image_type(*bytes); !type.empty()) {
            done(AssetIcon{.content_type = std::string{type}, .bytes = std::move(*bytes)});
            return;
        }
    }
    if (const auto it = no_icon_until_.find(key); it != no_icon_until_.end()) {
        if (now_().ns < it->second) {
            done(std::unexpected{none});
            return;
        }
        no_icon_until_.erase(it);
    }
    auto& waiting = fetching_[key];
    waiting.push_back(std::move(done));
    if (waiting.size() > 1) {
        return;
    }
    const auto kind = found.mark->stock ? AssetImageKind::Stock : AssetImageKind::Coin;
    upstream_->image(kind, found.mark->image, [this, key](int status, std::string body) {
        if (status == 200 && body.size() <= k_asset_icon_max) {
            if (const auto type = image_type(body); !type.empty()) {
                // a failed write only means the next view asks the host again
                (void)icons_->write(key, body);
                finish_icon(key, AssetIcon{.content_type = std::string{type}, .bytes = std::move(body)});
                return;
            }
        }
        const bool answered = status >= 200 && status < 500 && status != 408 && status != 429;
        no_icon_until_[key] = now_().ns + (answered ? limits_.missing_ns : limits_.failed_ns);
        finish_icon(key,
                    std::unexpected{answered ? AssetError{404, "no image for this asset", {}}
                                             : AssetError{503, "image host is not answering", "worth retrying"}});
    });
}

void AssetDesk::finish_icon(const std::string& key, std::expected<AssetIcon, AssetError> result) {
    auto node = fetching_.extract(key);
    if (node.empty()) {
        return;
    }
    for (const auto& done : node.mapped()) {
        done(result);
    }
}

bool AssetRoutes::operator()(const http::Request& req, const AssetRespond& respond) const {
    if (!req.path.starts_with(k_prefix)) {
        return false;
    }
    auto symbol = req.path.substr(k_prefix.size());
    const bool icon = symbol.ends_with(k_icon_suffix);
    if (icon) {
        symbol.remove_suffix(k_icon_suffix.size());
    }
    if (symbol.empty() || symbol.find('/') != std::string_view::npos) {
        return false;
    }
    if (req.method != "GET") {
        respond(http::problem(405, "method not allowed", "this path takes GET"));
        return true;
    }
    if (icon) {
        desk_->icon(symbol, [respond](std::expected<AssetIcon, AssetError> result) {
            if (!result.has_value()) {
                respond(refusal(result.error()));
                return;
            }
            respond({.status = 200,
                     .content_type = std::move(result->content_type),
                     .body = std::move(result->bytes),
                     .cache_control = std::string{k_icon_cache}});
        });
        return true;
    }
    desk_->profile(symbol, [respond](const std::expected<AssetProfile, AssetError>& result) {
        if (!result.has_value()) {
            respond(refusal(result.error()));
            return;
        }
        std::string out;
        if (const auto ec = glz::write<k_opts>(*result, out); ec) {
            respond(http::problem(500, "serialisation failed"));
            return;
        }
        respond({.status = 200, .content_type = "application/json", .body = std::move(out)});
    });
    return true;
}

} // namespace raikiri::admin
