#include "aowis/native/block_store_prototype.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double millis(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: aowis-native-block-store-benchmark CANDIDATE_DIRECTORY [WRITE_COUNT]\n";
        return 2;
    }
    try {
        std::uint64_t writes = 100;
        if (argc == 3) {
            std::size_t consumed = 0;
            writes = std::stoull(argv[2], &consumed);
            if (consumed != std::string(argv[2]).size() || writes > 10000) {
                throw std::runtime_error("WRITE_COUNT must be 0..10000");
            }
        }
        const auto openStart = Clock::now();
        auto candidate = std::make_unique<aowis::native::BlockStorePrototype>(argv[1]);
        const double openMs = millis(openStart);
        const auto before = candidate->stats();
        const std::vector<aowis::native::Hash> keys = candidate->hashes();
        const std::size_t sampled = std::min<std::size_t>(10000, keys.size());
        const auto readStart = Clock::now();
        for (std::size_t i = 0; i < sampled; ++i) {
            const std::size_t n = (i * 2654435761ULL + 1013904223ULL) % keys.size();
            if (!candidate->get(keys[n]).has_value()) throw std::runtime_error("indexed key missing");
        }
        const double indexedReadMs = millis(readStart);
        std::vector<aowis::native::Bytes> additions;
        for (std::uint64_t i = 0; i < writes; ++i) {
            aowis::native::Bytes payload(128);
            payload[0] = 'B'; payload[1] = 'S'; payload[2] = '1';
            for (unsigned n = 0; n < 8; ++n) payload[n + 3] = static_cast<std::uint8_t>(i >> (56U - 8U * n));
            for (std::size_t n = 11; n < payload.size(); ++n) {
                payload[n] = static_cast<std::uint8_t>((i * 131U + n * 17U) & 255U);
            }
            additions.push_back(std::move(payload));
        }
        const auto appendStart = Clock::now();
        const std::vector<aowis::native::Hash> inserted = candidate->putBatch(additions);
        const double appendMs = millis(appendStart);
        const auto after = candidate->stats();
        candidate.reset();
        const auto reopenStart = Clock::now();
        aowis::native::BlockStorePrototype reopened(argv[1]);
        const double reopenMs = millis(reopenStart);
        for (std::size_t i = 0; i < inserted.size(); ++i) {
            if (reopened.get(inserted[i]) != additions[i]) throw std::runtime_error("batch not durable");
        }
        const auto verifyStart = Clock::now();
        reopened.verify();
        const double verifyMs = millis(verifyStart);
        std::cout << std::fixed << std::setprecision(3)
                  << "format=experimental-writable-block-pack-not-authoritative\n"
                  << "objects_before=" << before.objects << "\n"
                  << "objects_after=" << after.objects << "\n"
                  << "pack_bytes_after=" << after.packBytes << "\n"
                  << "index_bytes_after=" << after.indexBytes << "\n"
                  << "indexed_reads=" << sampled << "\n"
                  << "open_ms=" << openMs << "\n"
                  << "indexed_read_ms=" << indexedReadMs << "\n"
                  << "append_objects=" << writes << "\n"
                  << "append_commit_ms=" << appendMs << "\n"
                  << "reopen_ms=" << reopenMs << "\n"
                  << "full_verify_ms=" << verifyMs << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Block store prototype failed: " << e.what() << '\n';
        return 1;
    }
}
