#include "aowis/native/native_store.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using aowis::native::Bytes;
using aowis::native::Change;
using aowis::native::Key;
using aowis::native::Repository;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::uint64_t positiveInteger(const char* text) {
    const std::string input(text);
    std::size_t consumed = 0;
    const std::uint64_t result = std::stoull(input, &consumed);
    if (consumed != input.size() || result == 0) {
        throw std::runtime_error("expected a positive integer: " + input);
    }
    return result;
}

Key keyFor(std::uint64_t number) {
    // Deterministically scatter UUIDs across the Merkle trie.
    number ^= number >> 30U;
    number *= 0xbf58476d1ce4e5b9ULL;
    number ^= number >> 27U;
    number *= 0x94d049bb133111ebULL;
    number ^= number >> 31U;
    Key key{};
    for (unsigned i = 0; i < 8; ++i) {
        key[i] = static_cast<std::uint8_t>(number >> (8U * i));
        key[i + 8] = static_cast<std::uint8_t>((number ^ 0x9e3779b97f4a7c15ULL) >> (8U * i));
    }
    return key;
}

Bytes valueFor(std::uint64_t object, std::uint64_t revision) {
    // Identity and version are embedded so values do not deduplicate every 256 records.
    Bytes payload(128);
    for (unsigned i = 0; i < 8; ++i) {
        payload[i] = static_cast<std::uint8_t>(object >> (i * 8U));
        payload[8 + i] = static_cast<std::uint8_t>(revision >> (i * 8U));
    }
    std::uint64_t state = (object + 1ULL) ^ ((revision + 1ULL) * 0xd6e8feb86659fd93ULL);
    for (std::size_t i = 16; i < payload.size(); ++i) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        payload[i] = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dULL) >> 56U);
    }
    return payload;
}

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t residentBytes() {
    // Linux VmRSS is the current resident set, unlike ru_maxrss (process-wide high-water mark).
    std::ifstream status("/proc/self/status");
    std::string name;
    while (status >> name) {
        if (name == "VmRSS:") {
            std::uint64_t kib = 0;
            std::string unit;
            status >> kib >> unit;
            if (unit != "kB") throw std::runtime_error("unexpected Linux VmRSS unit");
            return kib * 1024;
        }
        std::string rest;
        std::getline(status, rest);
    }
    throw std::runtime_error("/proc/self/status lacks VmRSS");
}

std::uint64_t peakBytes() {
    std::ifstream status("/proc/self/status");
    std::string name;
    while (status >> name) {
        if (name == "VmHWM:") {
            std::uint64_t kib = 0;
            std::string unit;
            status >> kib >> unit;
            if (unit != "kB") throw std::runtime_error("unexpected Linux VmHWM unit");
            return kib * 1024;
        }
        std::string rest;
        std::getline(status, rest);
    }
    throw std::runtime_error("/proc/self/status lacks VmHWM");
}

void metric(const std::string& name, std::uint64_t value) {
    std::cout << name << '=' << value << '\n';
}
void metric(const std::string& name, double value) {
    std::cout << name << '=' << std::fixed << std::setprecision(3) << value << '\n';
}

void checkRepository(const fs::path& path) {
    if (!fs::is_directory(path) ||
        (!fs::exists(path / "objects.pack") && !fs::exists(path / "objects.current"))) {
        throw std::runtime_error("expected an existing native repository: " + path.string());
    }
}

std::uint64_t indexBytes(const fs::path& path) {
    std::uint64_t bytes = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name == "objects.idx" || (name.starts_with("objects.") && name.ends_with(".idx"))) {
            bytes += entry.file_size();
        }
    }
    return bytes;
}

void repoMetrics(Repository& repo, const fs::path& path) {
    metric("physical_objects", repo.objectCount());
    metric("pack_bytes", repo.packSize());
    metric("index_bytes", indexBytes(path));
    std::cout << "head=" << aowis::native::toHex(repo.head("main").id) << '\n';
}

void prepare(const fs::path& path, std::uint64_t minimum, std::uint64_t keys,
             std::uint64_t batch) {
    if (fs::exists(path)) throw std::runtime_error("fixture path must not exist");
    if (keys > 1000000 || batch > keys || minimum > 50000000) {
        throw std::runtime_error("invalid fixture size (max 50 million physical objects)");
    }
    const Clock::time_point started = Clock::now();
    Repository repo(path);
    repo.createBranch("main");
    std::vector<Change> changes;
    changes.reserve(static_cast<std::size_t>(keys));
    for (std::uint64_t i = 0; i < keys; ++i) {
        changes.push_back(Change{keyFor(i), valueFor(i, 0)});
    }
    repo.commit("main", changes);
    changes.clear();
    std::uint64_t revision = 0;
    while (repo.objectCount() < minimum) {
        ++revision;
        for (std::uint64_t i = 0; i < batch; ++i) {
            const std::uint64_t object = ((revision - 1ULL) * batch + i) % keys;
            changes.push_back(Change{keyFor(object), valueFor(object, revision)});
        }
        repo.commit("main", changes);
        changes.clear();
    }
    std::cout << "phase=prepare\n";
    metric("target_objects", minimum);
    metric("keys", keys);
    metric("batch", batch);
    metric("revisions", revision + 1);
    metric("duration_ms", msSince(started));
    repoMetrics(repo, path);
    metric("rss_bytes", residentBytes());
    metric("peak_rss_bytes", peakBytes());
}

void probe(const fs::path& path, std::uint64_t keys, std::uint64_t samples) {
    checkRepository(path);
    const std::uint64_t before = residentBytes();
    const Clock::time_point started = Clock::now();
    Repository repo(path);
    const double openMs = msSince(started);
    const std::uint64_t afterOpen = residentBytes();
    const Clock::time_point hitStart = Clock::now();
    for (std::uint64_t i = 0; i < samples; ++i) {
        if (!repo.get("main", keyFor((i * 7919ULL) % keys)).has_value()) {
            throw std::runtime_error("expected fixture object missing");
        }
    }
    const double hitMs = msSince(hitStart);
    const Clock::time_point missStart = Clock::now();
    for (std::uint64_t i = 0; i < samples; ++i) {
        if (repo.get("main", keyFor(keys + 1000001ULL + i)).has_value()) {
            throw std::runtime_error("unexpected fixture object");
        }
    }
    const double missMs = msSince(missStart);
    std::cout << "phase=probe\n";
    metric("keys", keys);
    metric("samples", samples);
    metric("open_ms", openMs);
    metric("lookup_hit_ms", hitMs);
    metric("lookup_miss_ms", missMs);
    metric("rss_before_bytes", before);
    metric("rss_after_open_bytes", afterOpen);
    metric("rss_after_probe_bytes", residentBytes());
    metric("peak_rss_bytes", peakBytes());
    repoMetrics(repo, path);
}

void writeProbe(const fs::path& path, std::uint64_t keys, std::uint64_t batch,
                std::uint64_t rounds) {
    checkRepository(path);
    if (batch > keys || rounds > 100000) throw std::runtime_error("invalid write probe dimensions");
    Repository repo(path);
    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(rounds));
    const std::uint64_t initialCount = repo.objectCount();
    const aowis::native::Hash oldHead = repo.head("main").id;
    // Use a unique revision-version marker per run, preventing dedup across repeated probes.
    std::uint64_t nonce = 0;
    for (unsigned i = 0; i < 8; ++i) nonce = (nonce << 8U) | oldHead[i];
    for (std::uint64_t round = 1; round <= rounds; ++round) {
        std::vector<Change> edits;
        edits.reserve(static_cast<std::size_t>(batch));
        for (std::uint64_t i = 0; i < batch; ++i) {
            const std::uint64_t object = ((round - 1ULL) * batch + i) % keys;
            edits.push_back(Change{keyFor(object), valueFor(object, nonce + round)});
        }
        const Clock::time_point start = Clock::now();
        repo.commit("main", edits);
        timings.push_back(msSince(start));
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "phase=write\n";
    metric("batch", batch);
    metric("rounds", rounds);
    metric("write_mean_ms", std::accumulate(timings.begin(), timings.end(), 0.0) /
                              static_cast<double>(timings.size()));
    metric("write_p95_ms", timings[static_cast<std::size_t>(0.95 * static_cast<double>(rounds - 1))]);
    metric("objects_added", repo.objectCount() - initialCount);
    metric("rss_bytes", residentBytes());
    metric("peak_rss_bytes", peakBytes());
    repoMetrics(repo, path);
}

void compactProbe(const fs::path& path) {
    checkRepository(path);
    Repository repo(path);
    const Clock::time_point started = Clock::now();
    const aowis::native::CompactionStats stats = repo.compact();
    std::cout << "phase=compact\n";
    metric("duration_ms", msSince(started));
    metric("objects_before", stats.objectsBefore);
    metric("objects_after", stats.objectsAfter);
    metric("pack_bytes_before", stats.packBytesBefore);
    metric("pack_bytes_after", stats.packBytesAfter);
    metric("rss_bytes", residentBytes());
    metric("peak_rss_bytes", peakBytes());
    repoMetrics(repo, path);
}

void multiProbe(int argc, char** argv) {
    if (argc < 4) throw std::runtime_error("multi requires at least two distinct repositories");
    std::vector<std::unique_ptr<Repository>> repositories;
    std::set<fs::path> uniquePaths;
    const std::uint64_t before = residentBytes();
    double totalMs = 0;
    for (int i = 2; i < argc; ++i) {
        const fs::path path(argv[i]);
        checkRepository(path);
        if (!uniquePaths.insert(fs::canonical(path)).second) {
            throw std::runtime_error("multi requires distinct repository directories");
        }
        const Clock::time_point started = Clock::now();
        repositories.push_back(std::make_unique<Repository>(path));
        totalMs += msSince(started);
        (void)repositories.back()->head("main");
        metric("rss_project_" + std::to_string(i - 1) + "_bytes", residentBytes());
    }
    std::cout << "phase=multi\n";
    metric("projects", static_cast<std::uint64_t>(repositories.size()));
    metric("duration_ms", totalMs);
    metric("rss_before_bytes", before);
    metric("rss_bytes", residentBytes());
    metric("peak_rss_bytes", peakBytes());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("missing benchmark mode");
        const std::string mode(argv[1]);
        if (mode == "prepare" && argc == 6) {
            prepare(argv[2], positiveInteger(argv[3]), positiveInteger(argv[4]), positiveInteger(argv[5]));
        } else if (mode == "probe" && argc == 5) {
            probe(argv[2], positiveInteger(argv[3]), positiveInteger(argv[4]));
        } else if (mode == "write" && argc == 6) {
            writeProbe(argv[2], positiveInteger(argv[3]), positiveInteger(argv[4]), positiveInteger(argv[5]));
        } else if (mode == "compact" && argc == 3) {
            compactProbe(argv[2]);
        } else if (mode == "multi" && argc >= 4) {
            multiProbe(argc, argv);
        } else {
            throw std::runtime_error("Usage: aowis-native-index-scaling-benchmark\n"
                "  prepare <NEW_REPOSITORY> <MIN_PHYSICAL_OBJECTS> <KEYS> <EDIT_BATCH>\n"
                "  probe <EXISTING_REPOSITORY> <KEYS> <READ_SAMPLES>\n"
                "  write <EXISTING_REPOSITORY> <KEYS> <EDIT_BATCH> <ROUNDS>\n"
                "  compact <EXISTING_REPOSITORY>\n"
                "  multi <REPOSITORY_1> <REPOSITORY_2> [MORE_REPOSITORIES ...]");
        }
    } catch (const std::exception& error) {
        std::cerr << "Physical index benchmark error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
