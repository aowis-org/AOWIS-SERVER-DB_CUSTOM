#include "aowis/native/model_record.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace aowis::native {
namespace {

void append16(Bytes& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append32(Bytes& output, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        output.push_back(static_cast<std::uint8_t>((value >> (24U - 8U * i)) & 0xffU));
    }
}

std::uint16_t read16(const Bytes& bytes, std::size_t& cursor) {
    if (cursor > bytes.size() || bytes.size() - cursor < 2) {
        throw std::runtime_error("truncated native model record");
    }
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<unsigned>(bytes[cursor]) << 8U) | bytes[cursor + 1]);
    cursor += 2;
    return value;
}

std::uint32_t read32(const Bytes& bytes, std::size_t& cursor) {
    if (cursor > bytes.size() || bytes.size() - cursor < 4) {
        throw std::runtime_error("truncated native model record");
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8U) | bytes[cursor++];
    return value;
}

} // namespace

Bytes encodeModelRecord(const ModelRecord& record) {
    if (record.kind == 0) throw std::runtime_error("model record kind must be nonzero");
    if (record.references.size() > std::numeric_limits<std::uint32_t>::max() ||
        record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("model record exceeds encoding limit");
    }
    // References are a dependency *set*, not an ordered list of model fields.
    // The roles/order of pipe endpoints remain in the opaque canonical payload.
    std::vector<Key> references = record.references;
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    Bytes result{'A', 'O', 'M', 'D', 1};
    append16(result, record.kind);
    append32(result, static_cast<std::uint32_t>(references.size()));
    for (const Key& reference : references) {
        result.insert(result.end(), reference.begin(), reference.end());
    }
    append32(result, static_cast<std::uint32_t>(record.payload.size()));
    result.insert(result.end(), record.payload.begin(), record.payload.end());
    return result;
}

ModelRecord decodeModelRecord(const Bytes& bytes) {
    if (bytes.size() < 15 || !std::equal(bytes.begin(), bytes.begin() + 5,
                                         "AOMD\x01")) {
        throw std::runtime_error("invalid native model record envelope");
    }
    std::size_t cursor = 5;
    ModelRecord result{};
    result.kind = read16(bytes, cursor);
    if (result.kind == 0) throw std::runtime_error("model record kind must be nonzero");
    const std::uint32_t references = read32(bytes, cursor);
    if (references > (bytes.size() - cursor) / 16) {
        throw std::runtime_error("truncated native model references");
    }
    result.references.reserve(references);
    for (std::uint32_t i = 0; i < references; ++i) {
        Key reference{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), 16, reference.begin());
        result.references.push_back(reference);
        cursor += 16;
    }
    const std::uint32_t payloadSize = read32(bytes, cursor);
    if (payloadSize != bytes.size() - cursor) {
        throw std::runtime_error("invalid native model payload length");
    }
    result.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    return result;
}

} // namespace aowis::native
