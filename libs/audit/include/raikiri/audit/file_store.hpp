#pragma once

#include <raikiri/audit/query.hpp>

#include <string>

namespace raikiri::audit {

// `root` holds one directory per stream; a missing root is empty, since a writer may not have started
class FileStore final : public EntryStore {
public:
    explicit FileStore(std::string root) : root_{std::move(root)} {}

    [[nodiscard]] std::vector<std::string> streams() const override;
    [[nodiscard]] std::vector<std::uint32_t> days(const std::string& stream) const override;

    // lines that will not parse are skipped here; verify is what reports them
    void scan_day(const std::string& stream,
                  std::uint32_t day,
                  const std::function<void(const EntryView&)>& visit) const override;
    [[nodiscard]] ChainReport verify(const std::string& stream, std::uint32_t day) const override;

private:
    std::string root_;
};

} // namespace raikiri::audit
