#include <raikiri/books/book.hpp>
#include <raikiri/clientapi/infrastructure/stores.hpp>
#include <raikiri/net/signer.hpp>

#include <openssl/crypto.h>

#include <array>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

#include "json.hpp"

namespace raikiri::clientapi {

namespace wire {

struct RateFile {
    std::optional<std::uint32_t> requests_per_second{};
    std::optional<std::uint32_t> burst{};
    std::optional<std::uint32_t> mutations_per_second{};
    std::optional<std::uint32_t> mutation_burst{};
};

struct KeyFileEntry {
    std::string id{};
    std::string client_id{};
    std::string secret{};
    std::vector<std::string> permissions{};
    std::vector<std::string> accounts{};
    std::vector<std::string> symbols{};
    std::optional<RateFile> rate{};
    std::optional<std::vector<std::string>> ip_allowlist{};
    std::optional<std::uint32_t> max_streams{};
    std::optional<bool> disabled{};
};

struct ExtensionFile {
    std::string name{};
    std::uint16_t port{0};
    std::optional<std::string> permission{};
    std::optional<std::string> client_field{};
    std::vector<std::string> routes{};
};

struct KeyFile {
    std::vector<KeyFileEntry> keys{};
    std::optional<std::vector<ExtensionFile>> extensions{};
};

} // namespace wire

namespace {

constexpr std::size_t k_min_secret_bytes = 32;

std::optional<Permission> permission_of(std::string_view name) noexcept {
    if (name == "read") {
        return Permission::Read;
    }
    if (name == "trade") {
        return Permission::Trade;
    }
    if (name == "algo") {
        return Permission::Algo;
    }
    return std::nullopt;
}

} // namespace

std::expected<std::size_t, std::string> KeyFileDirectory::load() {
    std::ifstream in{path_};
    if (!in) {
        return std::unexpected("cannot read " + path_.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    wire::KeyFile file{};
    if (const auto ec = glz::read_json(file, text.str()); ec) {
        return std::unexpected(path_.string() + " does not parse: " + glz::format_error(ec, text.str()));
    }
    auto table = std::make_shared<Table>();
    for (const auto& entry : file.keys) {
        const auto named = "key " + entry.id + ": ";
        if (entry.id.empty() || entry.client_id.empty()) {
            return std::unexpected("a key needs an id and a client_id");
        }
        // it becomes a book's name, client:<id>
        if (!books::valid_label(entry.client_id)) {
            return std::unexpected(named + "the client_id is not a valid book label");
        }
        if (entry.accounts.empty() || entry.symbols.empty()) {
            return std::unexpected(named + "accounts and symbols must be listed; use \"*\" for all");
        }
        if (entry.disabled.value_or(false)) {
            continue;
        }
        const auto ref = config::SecretRef::parse(entry.secret);
        if (!ref.has_value()) {
            return std::unexpected(named + "the secret is not a reference");
        }
        if (ref->scheme != config::SecretRef::Scheme::Env && ref->scheme != config::SecretRef::Scheme::File &&
            ref->scheme != config::SecretRef::Scheme::Exec) {
            return std::unexpected(named + "the secret must be an env:, file: or exec: reference");
        }
        auto secret = std::make_unique<config::Secret>();
        if (const auto resolved = secrets_->resolve(*ref, *secret); !resolved.has_value()) {
            return std::unexpected(named +
                                   "the secret did not resolve: " + std::string{config::to_string(resolved.error())});
        }
        if (secret->size() < k_min_secret_bytes) {
            return std::unexpected(named + "the secret is shorter than 32 bytes");
        }
        auto key = std::make_shared<ApiKey>();
        key->id = entry.id;
        key->client_id = entry.client_id;
        key->accounts = entry.accounts;
        key->symbols = entry.symbols;
        for (const auto& name : entry.permissions) {
            const auto permission = permission_of(name);
            if (!permission.has_value()) {
                return std::unexpected(std::format("{}unknown permission {}", named, name));
            }
            key->permissions |= static_cast<std::uint8_t>(*permission);
        }
        const auto given = entry.rate.value_or(wire::RateFile{});
        key->rate.requests_per_second = given.requests_per_second.value_or(key->rate.requests_per_second);
        key->rate.burst = given.burst.value_or(key->rate.burst);
        key->rate.mutations_per_second = given.mutations_per_second.value_or(key->rate.mutations_per_second);
        key->rate.mutation_burst = given.mutation_burst.value_or(key->rate.mutation_burst);
        for (const auto& text_range : entry.ip_allowlist.value_or(std::vector<std::string>{})) {
            const auto range = parse_ip_range(text_range);
            if (!range.has_value()) {
                return std::unexpected(std::format("{}bad address in ip_allowlist: {}", named, text_range));
            }
            key->ips.push_back(*range);
        }
        key->max_streams = entry.max_streams.value_or(key->max_streams);
        if (!table->emplace(entry.id, Entry{.key = std::move(key), .secret = std::move(secret)}).second) {
            return std::unexpected(named + "listed twice");
        }
    }
    std::vector<Extension> extensions;
    for (const auto& item : file.extensions.value_or(std::vector<wire::ExtensionFile>{})) {
        Extension extension{.name = item.name, .port = item.port, .client_field = item.client_field.value_or("client")};
        extension.permission = permission_of(item.permission.value_or("trade")).value_or(Permission::Trade);
        for (const auto& route : item.routes) {
            const auto space = route.find(' ');
            if (space == std::string::npos || route.size() < space + 2 || route[space + 1] != '/') {
                return std::unexpected("extension " + item.name + ": a route is \"METHOD /path\", not " + route);
            }
            extension.routes.push_back({.method = route.substr(0, space), .path = route.substr(space + 1)});
        }
        extensions.push_back(std::move(extension));
    }
    const auto count = table->size();
    const std::unique_lock lock{mutex_};
    table_ = std::move(table);
    extensions_ = std::move(extensions);
    return count;
}

std::shared_ptr<const ApiKey> KeyFileDirectory::find(std::string_view id) const {
    const std::shared_lock lock{mutex_};
    if (table_ == nullptr) {
        return nullptr;
    }
    const auto found = table_->find(id);
    return found == table_->end() ? nullptr : found->second.key;
}

bool KeyFileDirectory::verify(const ApiKey& key, std::string_view message, std::string_view signature) const {
    std::shared_ptr<const Table> table;
    {
        const std::shared_lock lock{mutex_};
        table = table_;
    }
    if (table == nullptr || signature.size() != net::k_hex_bytes) {
        return false;
    }
    const auto found = table->find(key.id);
    if (found == table->end()) {
        return false;
    }
    std::array<char, net::k_hex_bytes> expected{};
    const auto mine = net::hmac_sha256_hex(found->second.secret->view(), message, expected);
    return mine.size() == signature.size() && ::CRYPTO_memcmp(mine.data(), signature.data(), mine.size()) == 0;
}

std::vector<Extension> KeyFileDirectory::extensions() const {
    const std::shared_lock lock{mutex_};
    return extensions_;
}

} // namespace raikiri::clientapi
