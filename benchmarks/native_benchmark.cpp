#include "aowis/dvmdb/native_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Key;
using aowis::dvmdb::Repository;

Key keyFor(std::uint64_t number) {
    // Deterministic scattered UUID-like keys; not chronological UUIDs.
    number ^= number >> 30U;
    number *= 0xbf58476d1ce4e5b9ULL;
    number ^= number >> 27U;
    number *= 0x94d049bb133111ebULL;
    number ^= number >> 31U;
    Key result{};
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>((number >> (8U * i)) & 0xffU);
        result[i + 8] = static_cast<std::uint8_t>(((number ^ 0x891bc765354aa035ULL) >> (8U * i)) & 0xffU);
    }
    return result;
}

Bytes valueFor(std::uint64_t object, std::uint64_t revision, bool unique) {
    Bytes result(128);
    if (!unique) {
        // Reproduce the first prototype's highly deduplicated fixture.
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = static_cast<std::uint8_t>((object * 17ULL + revision * 13ULL + i) & 0xffU);
        }
        return result;
    }
    // Encode identity as well as deterministic pseudo-random bytes. The
    // original fixture repeated payloads every 256 objects; that concealed
    // the real cost of storing 100,000 distinct model records.
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(object >> (8U * i));
        result[i + 8] = static_cast<std::uint8_t>(revision >> (8U * i));
    }
    std::uint64_t state = (object ^ 0x243f6a8885a308d3ULL) +
                          (revision + 1ULL) * 0x9e3779b97f4a7c15ULL;
    for (std::size_t i = 16; i < result.size(); ++i) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        result[i] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    return result;
}

double elapsedMs(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

std::uint64_t integer(const char* input) {
    std::size_t consumed = 0;
    const std::string text(input);
    const std::uint64_t value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0) throw std::runtime_error("positive integers required");
    return value;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 6) {
        std::cerr << "Usage: aowis-dvmdb-benchmark <NEW_DIRECTORY> [objects=1000] [revisions=10] [edits=10] [payload=unique|legacy]\n"
                  << "Creates a new durable repository. No AOWIS model validation is included yet.\n";
        return 2;
    }
    try {
        const std::filesystem::path path(argv[1]);
        if (std::filesystem::exists(path)) throw std::runtime_error("benchmark path already exists");
        const std::uint64_t objects = argc >= 3 ? integer(argv[2]) : 1000;
        const std::uint64_t revisions = argc >= 4 ? integer(argv[3]) : 10;
        const std::uint64_t edits = argc >= 5 ? integer(argv[4]) : 10;
        const std::string payloadMode = argc >= 6 ? argv[5] : "unique";
        if (payloadMode != "unique" && payloadMode != "legacy") {
            throw std::runtime_error("payload must be unique or legacy");
        }
        const bool unique = payloadMode == "unique";
        if (objects > 1000000 || revisions > 10000 || edits > objects) {
            throw std::runtime_error("invalid size or edits exceed object count");
        }
        std::cout << std::fixed << std::setprecision(3);
        const std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
        Repository repo(path);
        repo.createBranch("main");
        const double opening = elapsedMs(opened);
        std::vector<Change> seed;
        seed.reserve(static_cast<std::size_t>(objects));
        for (std::uint64_t i = 0; i < objects; ++i) {
            seed.push_back(Change{keyFor(i), valueFor(i, 0, unique)});
        }
        const std::chrono::steady_clock::time_point importing = std::chrono::steady_clock::now();
        repo.commit("main", seed);
        const double importMs = elapsedMs(importing);
        repo.createBranch("baseline", "main");
        const std::chrono::steady_clock::time_point revising = std::chrono::steady_clock::now();
        for (std::uint64_t revision = 1; revision <= revisions; ++revision) {
            std::vector<Change> changes;
            for (std::uint64_t edit = 0; edit < edits; ++edit) {
                const std::uint64_t number = (edit * 101ULL + revision * 17ULL) % objects;
                changes.push_back(Change{keyFor(number), valueFor(number, revision, unique)});
            }
            repo.commit("main", changes);
        }
        const double revisionMs = elapsedMs(revising);
        const std::chrono::steady_clock::time_point comparing = std::chrono::steady_clock::now();
        const std::vector<aowis::dvmdb::Difference> diff = repo.diff("baseline", "main");
        const double diffMs = elapsedMs(comparing);
        const std::chrono::steady_clock::time_point scanning = std::chrono::steady_clock::now();
        const std::size_t scanned = repo.scan("main").size();
        const double scanMs = elapsedMs(scanning);
        if (scanned != objects) throw std::runtime_error("scan has incorrect object count");
        const std::uint64_t packBytes = repo.packSize();
        const std::uint64_t storedObjects = repo.objectCount();
        const aowis::dvmdb::Revision finalRevision = repo.head("main");
        const std::chrono::steady_clock::time_point reopening = std::chrono::steady_clock::now();
        Repository reopened(path);
        if (reopened.head("main").root != finalRevision.root) {
            throw std::runtime_error("reopened repository has a different final root");
        }
        const double reopenMs = elapsedMs(reopening);
        const std::chrono::steady_clock::time_point verifying = std::chrono::steady_clock::now();
        reopened.verifyPack();
        const double verifyMs = elapsedMs(verifying);
        std::cout << "engine=native-v0\npayload_mode=" << payloadMode << "\nobjects=" << objects
                  << "\nrevisions=" << revisions
                  << "\nedits_per_revision=" << edits
                  << "\nopen_ms=" << opening
                  << "\nimport_ms=" << importMs
                  << "\nrevision_total_ms=" << revisionMs
                  << "\nrevision_mean_ms=" << revisionMs / static_cast<double>(revisions)
                  << "\ndiff_ms=" << diffMs
                  << "\ndiff_objects=" << diff.size()
                  << "\nscan_ms=" << scanMs
                  << "\npack_bytes=" << packBytes
                  << "\npack_objects=" << storedObjects
                  << "\nindex_bytes=" << std::filesystem::file_size(path / "objects.idx")
                  << "\nreopen_ms=" << reopenMs << "\nverify_ms=" << verifyMs << '\n';
    } catch (const std::exception& failure) {
        std::cerr << "Native benchmark error: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
