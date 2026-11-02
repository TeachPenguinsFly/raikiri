#pragma once

#include <raikiri/clientapi/application/ports.hpp>
#include <raikiri/config/secret.hpp>

#include <cstdio>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace raikiri::clientapi {

// a json file naming each key's scope and where its secret lives; never the secret itself
class KeyFileDirectory final : public KeyDirectory {
public:
    KeyFileDirectory(std::filesystem::path path, config::SecretProvider& secrets)
        : path_{std::move(path)}, secrets_{&secrets} {}

    // all or nothing: a key that does not load leaves the keys in force as they were; the error names it, not a secret
    [[nodiscard]] std::expected<std::size_t, std::string> load();

    [[nodiscard]] std::shared_ptr<const ApiKey> find(std::string_view id) const override;
    [[nodiscard]] bool verify(const ApiKey& key, std::string_view message, std::string_view signature) const override;

    // what /api/v1/x/<name> may reach; read from the same file
    [[nodiscard]] std::vector<Extension> extensions() const;

private:
    struct Entry {
        std::shared_ptr<const ApiKey> key{};
        std::unique_ptr<config::Secret> secret{};
    };

    using Table = std::map<std::string, Entry, std::less<>>;

    std::filesystem::path path_;
    config::SecretProvider* secrets_;
    mutable std::shared_mutex mutex_;
    std::shared_ptr<const Table> table_;
    std::vector<Extension> extensions_;
};

// one json object per line; a line is synced before append returns
class FileLedger final : public LedgerLog {
public:
    explicit FileLedger(std::filesystem::path path) : path_{std::move(path)} {}

    ~FileLedger() override;

    [[nodiscard]] bool open();
    [[nodiscard]] bool append(const LedgerRecord& record) override;
    void replay(const std::function<void(const LedgerRecord&)>& visit) override;

private:
    std::filesystem::path path_;
    std::mutex mutex_;
    int fd_{-1};
};

} // namespace raikiri::clientapi
