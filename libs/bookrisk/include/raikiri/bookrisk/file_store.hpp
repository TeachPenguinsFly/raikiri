#pragma once

#include <raikiri/bookrisk/ports.hpp>

#include <string>

namespace raikiri::bookrisk {

// one json document, replaced whole with a rename after an fsync, so a crash leaves the old one or the new one
class FileSettingsStore final : public SettingsStore {
public:
    explicit FileSettingsStore(std::string path) : path_{std::move(path)} {}

    [[nodiscard]] std::expected<std::optional<DeskSettings>, std::string> load() override;
    [[nodiscard]] std::expected<void, std::string> save(const DeskSettings& settings) override;

private:
    std::string path_;
};

} // namespace raikiri::bookrisk
