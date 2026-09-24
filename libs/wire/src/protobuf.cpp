#include <raikiri/wire/protobuf.hpp>

namespace raikiri::wire::pb {

namespace {

// ten 7-bit groups cover a u64
constexpr std::size_t k_max_varint_bytes = 10;

} // namespace

std::optional<std::uint64_t> Reader::varint() noexcept {
    std::uint64_t out = 0;
    std::size_t shift = 0;
    for (std::size_t i = 0; i < k_max_varint_bytes; ++i) {
        if (pos_ >= buf_.size()) {
            return std::nullopt;
        }
        const auto byte = static_cast<std::uint8_t>(buf_[pos_++]);
        out |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return out;
        }
        shift += 7;
    }
    return std::nullopt;
}

std::optional<Field> Reader::next() noexcept {
    if (done()) {
        return std::nullopt;
    }
    const auto key = varint();
    if (!key.has_value()) {
        return std::nullopt;
    }
    Field out{};
    out.number = static_cast<std::uint32_t>(*key >> 3U);
    if (out.number == 0) {
        return std::nullopt; // field 0 is not assignable; the buffer is not a message
    }
    switch (*key & 7U) {
    case 0: {
        const auto value = varint();
        if (!value.has_value()) {
            return std::nullopt;
        }
        out.type = WireType::Varint;
        out.varint = *value;
        return out;
    }
    case 1: {
        if (buf_.size() - pos_ < 8) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf_[pos_ + i]))
                     << (8U * i);
        }
        pos_ += 8;
        out.type = WireType::Fixed64;
        out.varint = value;
        return out;
    }
    case 2: {
        const auto len = varint();
        if (!len.has_value() || *len > buf_.size() - pos_) {
            return std::nullopt;
        }
        out.type = WireType::Bytes;
        out.bytes = buf_.substr(pos_, static_cast<std::size_t>(*len));
        pos_ += static_cast<std::size_t>(*len);
        return out;
    }
    case 5: {
        if (buf_.size() - pos_ < 4) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf_[pos_ + i]))
                     << (8U * i);
        }
        pos_ += 4;
        out.type = WireType::Fixed32;
        out.varint = value;
        return out;
    }
    default:
        // groups (3, 4) and the unassigned 6 and 7 mean this is not a message we can read
        return std::nullopt;
    }
}

std::optional<std::string_view> bytes_field(std::string_view buffer,
                                            std::uint32_t number) noexcept {
    Reader reader{buffer};
    while (const auto field = reader.next()) {
        if (field->number == number && field->type == WireType::Bytes) {
            return field->bytes;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> varint_field(std::string_view buffer, std::uint32_t number) noexcept {
    Reader reader{buffer};
    while (const auto field = reader.next()) {
        if (field->number == number && field->type == WireType::Varint) {
            return field->varint;
        }
    }
    return std::nullopt;
}

} // namespace raikiri::wire::pb
