#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace aowis::dvmdb {

// RFC 9562 UUIDv7 wire bytes, in network (big-endian) order. Unlike an
// immutable Hash, this is a unique logical identifier, not a content digest.
struct UuidV7 {
    std::array<std::uint8_t, 16> bytes{};

    static std::optional<UuidV7> parse(std::string_view value);
    static UuidV7 generate();
    std::string toString() const;
    std::uint64_t unixMilliseconds() const noexcept;
    bool operator==(const UuidV7&) const = default;
    bool operator<(const UuidV7& other) const noexcept { return this->bytes < other.bytes; }
};

// The clock callback also allows deterministic backward-clock testing. The
// generator synchronizes calls in one process, preserving lexical ordering
// even when the wall clock repeats or moves backwards. Independent processes
// use independently seeded cryptographic random bits; the owning store must
// still reject duplicate IDs at its persistence boundary.
class UuidV7Generator final {
public:
    using Clock = std::function<std::uint64_t()>;
    explicit UuidV7Generator(Clock clock = {});
    UuidV7 generate();
private:
    Clock clock_;
    std::mutex mutex_;
    std::uint64_t lastMs_ = 0;
    std::uint64_t lastProcessId_ = 0;
    std::uint16_t randomA_ = 0;
    std::uint64_t randomB_ = 0;
    bool started_ = false;
};

// Distinct domain tags make it impossible to accidentally pass a branch ID
// where a revision ID is expected. ProjectService takes project IDs as
// strings because they arrive as HTTP path parameters.
struct ProjectIdTag {};
struct BranchIdTag {};
struct RevisionIdTag {};

template <typename Tag>
class ObjectId final {
public:
    explicit ObjectId(UuidV7 value) : value_(value) {}
    static ObjectId generate() { return ObjectId(UuidV7::generate()); }
    static std::optional<ObjectId> parse(std::string_view value) {
        const std::optional<UuidV7> parsed = UuidV7::parse(value);
        if (!parsed) return std::nullopt;
        return ObjectId(*parsed);
    }
    std::string toString() const { return this->value_.toString(); }
    const UuidV7& uuid() const noexcept { return this->value_; }
    bool operator==(const ObjectId&) const = default;
    bool operator<(const ObjectId& other) const noexcept { return this->value_ < other.value_; }
private:
    UuidV7 value_;
};

using ProjectId = ObjectId<ProjectIdTag>;
using BranchId = ObjectId<BranchIdTag>;
using RevisionId = ObjectId<RevisionIdTag>;

} // namespace aowis::dvmdb
