#include <raikiri/audit/chain.hpp>
#include <raikiri/audit/day_index.hpp>
#include <raikiri/audit/verify.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace raikiri::audit {

namespace internal {

constexpr std::size_t k_bloom_bits = k_bloom_words * 64;
constexpr std::size_t k_probes = 3;

struct Probes {
    std::uint64_t first{0};
    std::uint64_t step{0};
};

[[nodiscard]] Probes probes_of(IndexField field, std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL ^ static_cast<std::uint64_t>(field);
    hash *= 1099511628211ULL;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return {.first = hash, .step = (hash >> 32) | 1U};
}

void set_bits(IndexBlock& block, IndexField field, std::string_view value) noexcept {
    const auto probes = probes_of(field, value);
    for (std::size_t i = 0; i < k_probes; ++i) {
        const auto bit = (probes.first + (i * probes.step)) % k_bloom_bits;
        block.bloom[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }
}

} // namespace internal

void IndexBlock::add(const Entry& entry, std::uint64_t at, std::uint64_t line_bytes) noexcept {
    if (lines == 0) {
        offset = at;
        min_ts = entry.ts.ns;
        max_ts = entry.ts.ns;
    }
    min_ts = std::min(min_ts, entry.ts.ns);
    max_ts = std::max(max_ts, entry.ts.ns);
    bytes += line_bytes;
    ++lines;
    internal::set_bits(*this, IndexField::Actor, entry.actor);
    internal::set_bits(*this, IndexField::Action, entry.action);
    internal::set_bits(*this, IndexField::Subject, entry.subject);
}

bool IndexBlock::may_hold(IndexField field, std::string_view value) const noexcept {
    const auto probes = internal::probes_of(field, value);
    for (std::size_t i = 0; i < internal::k_probes; ++i) {
        const auto bit = (probes.first + (i * probes.step)) % internal::k_bloom_bits;
        if ((bloom[bit / 64] & (std::uint64_t{1} << (bit % 64))) == 0) {
            return false;
        }
    }
    return true;
}

std::string index_path(const std::string& stream_dir, std::uint32_t day) {
    return stream_dir + "/" + day_name(day) + ".idx";
}

std::vector<IndexBlock> read_index(const std::string& path, std::uint64_t file_bytes) {
    const auto content = internal::read_file(path);
    std::vector<IndexBlock> out;
    if (!content.has_value()) {
        return out;
    }
    out.reserve(content->size() / sizeof(IndexBlock));
    std::uint64_t covered = 0;
    for (std::size_t at = 0; at + sizeof(IndexBlock) <= content->size(); at += sizeof(IndexBlock)) {
        IndexBlock block{};
        std::memcpy(&block, content->data() + at, sizeof(IndexBlock));
        // order and bounds are all a damaged record could get wrong that a reader would act on
        if (block.bytes == 0 || block.offset < covered || block.offset + block.bytes > file_bytes) {
            break;
        }
        covered = block.offset + block.bytes;
        out.push_back(block);
    }
    return out;
}

std::vector<std::uint32_t> days_past(const std::string& stream_dir, std::uint32_t keep_days, std::uint32_t today) {
    std::vector<std::uint32_t> out;
    if (keep_days == 0) {
        return out;
    }
    for (const auto day : list_days(stream_dir)) {
        if (day + keep_days <= today) {
            out.push_back(day);
        }
    }
    return out;
}

void remove_day(const std::string& stream_dir, std::uint32_t day) {
    std::error_code ec;
    std::filesystem::remove(day_path(stream_dir, day), ec);
    std::filesystem::remove(index_path(stream_dir, day), ec);
}

} // namespace raikiri::audit
