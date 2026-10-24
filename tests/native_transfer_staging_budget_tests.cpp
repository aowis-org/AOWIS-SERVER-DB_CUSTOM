#include "aowis/native/transfer_staging_budget.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
using aowis::native::http::TransferStagingBudget;

void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

void testCapacity() {
    TransferStagingBudget budget(100);
    check(!budget.reserve(0).has_value(), "zero size should not reserve");
    check(!budget.reserve(101).has_value(), "oversized request accepted");
    std::optional<TransferStagingBudget::Reservation> first = budget.reserve(60);
    check(first.has_value() && first->bytes() == 60, "first reservation failed");
    check(!budget.reserve(41).has_value(), "oversubscription accepted");
    {
        std::optional<TransferStagingBudget::Reservation> second = budget.reserve(40);
        check(second.has_value(), "exact capacity rejected");
        check(budget.reservedBytes() == 100, "usage accounting wrong");
        check(!budget.reserve(1).has_value(), "full capacity exceeded");
    }
    check(budget.reservedBytes() == 60, "scope did not release reservation");
    first.reset();
    check(budget.reservedBytes() == 0, "release left reserved bytes");
}

void testMoveAndLifetime() {
    TransferStagingBudget budget(100);
    std::optional<TransferStagingBudget::Reservation> initial = budget.reserve(75);
    check(initial.has_value(), "reservation missing");
    TransferStagingBudget::Reservation moved(std::move(*initial));
    initial.reset();
    check(budget.reservedBytes() == 75, "move constructor released reservation early");
    {
        std::optional<TransferStagingBudget::Reservation> replacement = budget.reserve(20);
        check(replacement.has_value(), "second reservation missing");
        *replacement = std::move(moved);
        check(budget.reservedBytes() == 75, "move assignment did not release old slot");
    }
    check(budget.reservedBytes() == 0, "move assignment leaked budget");
    std::shared_ptr<TransferStagingBudget::Reservation> delayed;
    {
        std::unique_ptr<TransferStagingBudget> temporary =
            std::make_unique<TransferStagingBudget>(16);
        std::optional<TransferStagingBudget::Reservation> slot = temporary->reserve(16);
        delayed = std::make_shared<TransferStagingBudget::Reservation>(std::move(*slot));
    }
    delayed.reset(); // Must remain safe after the budget object is destroyed.
    bool rejected = false;
    try { TransferStagingBudget invalid(0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "zero-capacity budget accepted");
}

void testConcurrentReservations() {
    TransferStagingBudget budget(4);
    std::atomic<bool> go{false};
    std::atomic<unsigned> current{0};
    std::atomic<unsigned> maximum{0};
    std::atomic<unsigned> successes{0};
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 24; ++index) {
        threads.emplace_back([&]() {
            while (!go.load()) std::this_thread::yield();
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                std::optional<TransferStagingBudget::Reservation> slot = budget.reserve(1);
                if (!slot) { std::this_thread::yield(); continue; }
                const unsigned now = current.fetch_add(1) + 1;
                unsigned previous = maximum.load();
                while (now > previous && !maximum.compare_exchange_weak(previous, now)) {}
                successes.fetch_add(1);
                std::this_thread::yield();
                current.fetch_sub(1);
            }
        });
    }
    go.store(true);
    for (std::thread& worker : threads) worker.join();
    check(maximum.load() <= 4 && maximum.load() > 0, "concurrent budget exceeded");
    check(successes.load() > 0, "no concurrent reservation succeeded");
    check(budget.reservedBytes() == 0, "concurrent use leaked reservation");
}
} // namespace

int main() {
    try {
        testCapacity();
        testMoveAndLifetime();
        testConcurrentReservations();
        std::cout << "transfer staging budget tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transfer staging budget tests failed: " << error.what() << '\n';
        return 1;
    }
}
