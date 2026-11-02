#pragma once

#include <raikiri/audit/query.hpp>
#include <raikiri/audit/verify.hpp>

#include <map>
#include <string>
#include <utility>

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
    // a day holds gigabytes on a busy book: reads back in blocks, and only the blocks the day's index cannot rule out
    void scan_day_newest(const std::string& stream,
                         std::uint32_t day,
                         const ScanHint& hint,
                         const std::function<bool(const EntryView&)>& visit) const override;
    // polled, and a busy day takes seconds to hash: each poll walks only what was added since the last. a full walk is
    // auditcheck's, or the first after a restart
    [[nodiscard]] ChainReport verify(const std::string& stream, std::uint32_t day) const override;

private:
    std::string root_;
    mutable std::map<std::pair<std::string, std::uint32_t>, VerifiedPrefix> verified_;
};

} // namespace raikiri::audit
