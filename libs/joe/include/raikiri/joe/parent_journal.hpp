#pragma once

// parents go in the strategy journal beside the order events, so one replay brings back both in the order they happened

#include <raikiri/joe/desk.hpp>
#include <raikiri/journal/writer.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace raikiri::joe {

// above every generated message id; other readers of the journal pass over it
inline constexpr std::uint16_t k_parent_record_schema = 1001;

[[nodiscard]] std::string encode_parent_record(const ParentRecord& record);
[[nodiscard]] std::optional<ParentRecord> decode_parent_record(std::string_view text);

class JournalParentLog final : public ParentLog {
public:
    explicit JournalParentLog(journal::JournalWriter& journal) noexcept : journal_{&journal} {}

    void record(const ParentRecord& record) noexcept override;

private:
    journal::JournalWriter* journal_;
};

// how many records it read; the desk is ready for its first tick afterwards. `strategy_id` is joe's: no other order
// event can be a child
std::size_t
restore_desk(Desk& desk, const std::string& journal_dir, const std::string& prefix, std::uint16_t strategy_id);

} // namespace raikiri::joe
