#include <raikiri/clientapi/infrastructure/stores.hpp>
#include <raikiri/log/log.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>

#include "json.hpp"

namespace raikiri::clientapi {

FileLedger::~FileLedger() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool FileLedger::open() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    return fd_ >= 0;
}

bool FileLedger::append(const LedgerRecord& record) {
    auto line = internal::to_json(record);
    line += '\n';
    const std::scoped_lock lock{mutex_};
    if (fd_ < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < line.size()) {
        const auto n = ::write(fd_, line.data() + written, line.size() - written);
        if (n <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return ::fsync(fd_) == 0;
}

void FileLedger::replay(const std::function<void(const LedgerRecord&)>& visit) {
    std::ifstream in{path_};
    std::string line;
    std::size_t torn = 0;
    while (std::getline(in, line)) {
        LedgerRecord record{};
        if (!internal::from_backend(record, line)) {
            ++torn; // a crash can cut the last line short
            continue;
        }
        visit(record);
    }
    if (torn != 0) {
        RLOG_WARN("apid: {} ledger lines did not parse", torn);
    }
}

} // namespace raikiri::clientapi
