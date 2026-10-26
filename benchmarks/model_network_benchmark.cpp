#include "aowis/dvmdb/native_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#endif

namespace {

using aowis::dvmdb::Bytes;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;

// Uses the UUID layout of a deterministic test fixture, not real UUIDv7.
Key keyFor(std::uint8_t kind, std::uint64_t number) {
    Key result{};
    result[0] = kind;
    for (unsigned i = 0; i < 8; ++i) {
        result[15 - i] = static_cast<std::uint8_t>(number >> (8U * i));
    }
    return result;
}

// A unique, deterministic opaque placeholder for the shared AOWIS ModelCodec
// output. This is NOT canonical model serialization; benchmark only the
// native record envelope, reference validation and topology operations.
Bytes payloadFor(std::uint64_t number, std::uint64_t revision, std::size_t size) {
    Bytes result(size);
    std::uint64_t state = (number + 1ULL) * 0x9e3779b97f4a7c15ULL ^
                          (revision + 1ULL) * 0xbf58476d1ce4e5b9ULL;
    for (std::size_t i = 0; i < result.size(); ++i) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        result[i] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    for (unsigned i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(number >> (8U * i));
        result[i + 8] = static_cast<std::uint8_t>(revision >> (8U * i));
    }
    return result;
}

std::uint64_t positive(const char* value) {
    const std::string text(value);
    std::size_t parsed = 0;
    const std::uint64_t number = std::stoull(text, &parsed);
    if (parsed != text.size() || number == 0) throw std::runtime_error("positive integers required");
    return number;
}

double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: aowis-dvmdb-model-benchmark <NEW_DIRECTORY> [pipes=1000] [revisions=10] [edits=10]\n"
                  << "Synthetic network-shaped typed-model fixture, NOT a canonical model codec benchmark.\n";
        return 2;
    }
    try {
        const std::filesystem::path path(argv[1]);
        if (std::filesystem::exists(path)) throw std::runtime_error("benchmark path already exists");
        const std::uint64_t pipes = argc >= 3 ? positive(argv[2]) : 1000;
        const std::uint64_t revisions = argc >= 4 ? positive(argv[3]) : 10;
        const std::uint64_t edits = argc >= 5 ? positive(argv[4]) : 10;
        if (pipes > 1000000 || revisions > 10000 || edits > pipes) {
            throw std::runtime_error("invalid size or edits exceed pipe count");
        }
        std::cout << std::fixed << std::setprecision(3);
        const std::chrono::steady_clock::time_point opening = std::chrono::steady_clock::now();
        std::unique_ptr<Repository> repository = std::make_unique<Repository>(path);
        repository->createBranch("main");
        const double openMs = elapsed(opening);

        std::vector<ModelChange> initial;
        initial.reserve(static_cast<std::size_t>(pipes * 2 + 1));
        // Junctions and pipes are distinct entity kinds. Each pipe references
        // its two endpoints; the native commit validates both in one batch.
        for (std::uint64_t i = 0; i <= pipes; ++i) {
            initial.push_back({keyFor(1, i), ModelRecord{1, {}, payloadFor(i, 0, 160)}});
        }
        for (std::uint64_t i = 0; i < pipes; ++i) {
            initial.push_back({keyFor(2, i),
                ModelRecord{4, {keyFor(1, i), keyFor(1, i + 1)}, payloadFor(i, 0, 192)}});
        }
        const std::chrono::steady_clock::time_point importing = std::chrono::steady_clock::now();
        repository->commitModel("main", std::move(initial));
        const double importMs = elapsed(importing);
        const std::chrono::steady_clock::time_point checkingReferences = std::chrono::steady_clock::now();
        const Key indexedNode = keyFor(1, 0);
        const std::size_t incomingReferences = repository->referencingModel("main", indexedNode).size();
        const double referenceLookupMs = elapsed(checkingReferences);
        const std::chrono::steady_clock::time_point checkingDeletion = std::chrono::steady_clock::now();
        bool deletionRejected = false;
        try { repository->commitModel("main", {{indexedNode, std::nullopt}}); }
        catch (const std::runtime_error&) { deletionRejected = true; }
        const double deletionCheckMs = elapsed(checkingDeletion);
        if (!deletionRejected || incomingReferences != 1) {
            throw std::runtime_error("indexed endpoint deletion check failed");
        }
        repository->createBranch("baseline", "main");
        const std::chrono::steady_clock::time_point revising = std::chrono::steady_clock::now();
        for (std::uint64_t revision = 1; revision <= revisions; ++revision) {
            std::vector<ModelChange> changes;
            changes.reserve(static_cast<std::size_t>(edits));
            for (std::uint64_t j = 0; j < edits; ++j) {
                const std::uint64_t pipe = (j * 101ULL + revision * 17ULL) % pipes;
                changes.push_back({keyFor(2, pipe), ModelRecord{4,
                    {keyFor(1, pipe), keyFor(1, pipe + 1)}, payloadFor(pipe, revision, 192)}});
            }
            repository->commitModel("main", std::move(changes));
        }
        const double revisionMs = elapsed(revising);
        const std::chrono::steady_clock::time_point comparing = std::chrono::steady_clock::now();
        const std::size_t differenceCount = repository->diff("baseline", "main").size();
        const double diffMs = elapsed(comparing);
        const std::chrono::steady_clock::time_point reading = std::chrono::steady_clock::now();
        std::uint64_t checked = 0;
        for (const std::pair<Key, Bytes>& entry : repository->scan("main")) {
            const ModelRecord record = aowis::dvmdb::decodeModelRecord(entry.second);
            if (record.kind != 1 && record.kind != 4) throw std::runtime_error("unexpected model kind");
            if (record.kind == 4 && record.references.size() != 2) {
                throw std::runtime_error("pipe lost endpoint references");
            }
            ++checked;
        }
        const double scanMs = elapsed(reading);
        if (checked != pipes * 2 + 1) throw std::runtime_error("network entity count differs");
        const aowis::dvmdb::Revision last = repository->head("main");
        const std::uint64_t packBytes = repository->packSize();
        const std::uint64_t packObjects = repository->objectCount();
        repository.reset();
        const std::chrono::steady_clock::time_point reopening = std::chrono::steady_clock::now();
        Repository reopened(path);
        if (reopened.head("main").root != last.root) throw std::runtime_error("reopen root mismatch");
        if (!reopened.getModel("main", keyFor(2, pipes - 1))) {
            throw std::runtime_error("missing reopened pipe");
        }
        const double reopenMs = elapsed(reopening);
        const std::chrono::steady_clock::time_point verifying = std::chrono::steady_clock::now();
        reopened.verifyPack();
        const double verifyMs = elapsed(verifying);
        std::cout << "engine=native-pack\n"
                  << "fixture=synthetic-hydraulic-topology\n"
                  << "codec=synthetic-not-serialized\n"
                  << "pipes=" << pipes << "\nnodes=" << pipes + 1
                  << "\nobjects=" << checked << "\nrevisions=" << revisions
                  << "\nedits_per_revision=" << edits << "\nopen_ms=" << openMs
                  << "\nincoming_refs=" << incomingReferences
                  << "\nreference_lookup_ms=" << referenceLookupMs
                  << "\nindexed_delete_reject_ms=" << deletionCheckMs
                  << "\nimport_ms=" << importMs << "\nrevision_total_ms=" << revisionMs
                  << "\nrevision_mean_ms=" << revisionMs / static_cast<double>(revisions)
                  << "\ndiff_ms=" << diffMs << "\ndiff_objects=" << differenceCount
                  << "\nscan_ms=" << scanMs << "\npack_bytes=" << packBytes
                  << "\npack_objects=" << packObjects
                  << "\nindex_bytes=" << std::filesystem::file_size(path / "objects.idx")
                  << "\nreopen_ms=" << reopenMs << "\nverify_ms=" << verifyMs << '\n';
#ifdef __linux__
        // Linux reports ru_maxrss in KiB. Capture the whole benchmark process
        // (including canonical codec allocations, scanning and reopening).
        struct rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) == 0) {
            std::cout << "peak_rss_kib=" << usage.ru_maxrss << '\n';
        }
#endif
    } catch (const std::exception& error) {
        std::cerr << "Native model benchmark error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
