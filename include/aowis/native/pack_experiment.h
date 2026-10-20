#pragma once

#include <cstdint>
#include <filesystem>

namespace aowis::native {

// An OFFLINE, non-authoritative physical-layout experiment. Writes a separate
// block pack and index; never changes the source repository or revision hashes.
struct PackExperimentStats {
    std::uint64_t objects = 0;
    std::uint64_t blocks = 0;
    std::uint64_t originalPackBytes = 0;
    std::uint64_t originalIndexBytes = 0;
    std::uint64_t candidatePackBytes = 0;
    std::uint64_t candidateIndexBytes = 0;
    std::uint64_t sampleReads = 0;
    std::uint32_t sampleTrials = 0;
    double sourceReadMs = 0;
    double candidateReadMs = 0;
    double buildMs = 0;
    double fullVerifyMs = 0;
};

PackExperimentStats runPackExperiment(const std::filesystem::path& repositoryDirectory,
                                      const std::filesystem::path& outputDirectory,
                                      std::uint32_t targetBlockBytes = 65536);

// Independent verification. Recomputes the block digests and every object's
// SHA-256, and checks the derived index against the block contents.
void verifyPackExperiment(const std::filesystem::path& outputDirectory);
// Recreate a missing derived index entirely from verified block data.
void rebuildPackExperimentIndex(const std::filesystem::path& outputDirectory);

} // namespace aowis::native
