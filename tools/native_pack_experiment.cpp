#include "aowis/native/pack_experiment.h"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <limits>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: aowis-native-pack-experiment REPOSITORY NEW_OUTPUT_DIRECTORY [BLOCK_BYTES]\n";
        return 2;
    }
    try {
        std::uint32_t target = 65536;
        if (argc == 4) {
            const std::string argument(argv[3]);
            std::size_t consumed = 0;
            const unsigned long long parsed = std::stoull(argument, &consumed);
            if (consumed != argument.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("invalid target block size");
            }
            target = static_cast<std::uint32_t>(parsed);
        }
        const aowis::native::PackExperimentStats report =
            aowis::native::runPackExperiment(argv[1], argv[2], target);
        const std::uint64_t original = report.originalPackBytes + report.originalIndexBytes;
        const std::uint64_t candidate = report.candidatePackBytes + report.candidateIndexBytes;
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "format=offline-block-candidate-not-production\n"
                  << "objects=" << report.objects << "\n"
                  << "blocks=" << report.blocks << "\n"
                  << "old_pack_bytes=" << report.originalPackBytes << "\n"
                  << "old_index_bytes=" << report.originalIndexBytes << "\n"
                  << "candidate_pack_bytes=" << report.candidatePackBytes << "\n"
                  << "candidate_index_bytes=" << report.candidateIndexBytes << "\n"
                  << "total_saved_bytes=" << (original - candidate) << "\n"
                  << "total_saved_pct=" << ((original - candidate) * 100.0 / original) << "\n"
                  << "build_ms=" << report.buildMs << "\n"
                  << "full_verify_ms=" << report.fullVerifyMs << "\n"
                  << "sample_reads=" << report.sampleReads << "\n"
                  << "sample_trials=" << report.sampleTrials << "\n"
                  << "sample_mode=prelocated-warm-cache-verified-paired-median\n"
                  << "source_sample_read_ms=" << report.sourceReadMs << "\n"
                  << "candidate_sample_read_ms=" << report.candidateReadMs << "\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Native pack experiment failed: " << failure.what() << '\n';
        return 1;
    }
}
