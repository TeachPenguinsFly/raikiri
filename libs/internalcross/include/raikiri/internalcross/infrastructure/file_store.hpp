#pragma once

#include <raikiri/internalcross/application/ports.hpp>

#include <string>

namespace raikiri::internalcross {

class FileSettingsStore final : public SettingsStore {
public:
    explicit FileSettingsStore(std::string path) : path_{std::move(path)} {}

    [[nodiscard]] std::expected<std::optional<Settings>, std::string> load() override;
    [[nodiscard]] std::expected<void, std::string> save(const Settings& settings) override;

private:
    std::string path_;
};

} // namespace raikiri::internalcross
