#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace aowis::dvmdb::http {

// A process-local ceiling on HTTP bundle staging, shared across all projects.
// Reservations conservatively charge the configured maximum transfer size,
// not the bytes currently written. This rejects excessive concurrent staging
// BEFORE opening a scratch file and accounts for stalled clients and response
// providers that outlive a route handler. Internal native verification scratch
// is separate and is NOT included in this budget.
class TransferStagingBudget final {
private:
    struct State {
        explicit State(std::uint64_t maximum) : maximumBytes(maximum) {}
        std::mutex mutex;
        std::uint64_t maximumBytes;
        std::uint64_t reservedBytes = 0;
    };

public:
    class Reservation final {
    public:
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                release();
                this->state_ = std::move(other.state_);
                this->bytes_ = std::exchange(other.bytes_, 0);
            }
            return *this;
        }
        ~Reservation() { release(); }
        std::uint64_t bytes() const noexcept { return this->bytes_; }

    private:
        friend class TransferStagingBudget;
        Reservation(std::shared_ptr<State> state, std::uint64_t bytes) noexcept
            : state_(std::move(state)), bytes_(bytes) {}
        void release() noexcept {
            if (!this->state_) return;
            std::shared_ptr<State> state = std::move(this->state_);
            {
                const std::lock_guard<std::mutex> lock(state->mutex);
                state->reservedBytes -= this->bytes_;
            }
            this->bytes_ = 0;
        }
        std::shared_ptr<State> state_;
        std::uint64_t bytes_ = 0;
    };

    explicit TransferStagingBudget(std::uint64_t maximumBytes)
        : state_(std::make_shared<State>(maximumBytes)) {
        if (maximumBytes == 0) throw std::invalid_argument("staging budget must be positive");
    }
    TransferStagingBudget(const TransferStagingBudget&) = delete;
    TransferStagingBudget& operator=(const TransferStagingBudget&) = delete;

    std::optional<Reservation> reserve(std::uint64_t bytes) {
        if (bytes == 0) return std::nullopt;
        const std::lock_guard<std::mutex> lock(this->state_->mutex);
        if (bytes > this->state_->maximumBytes - this->state_->reservedBytes)
            return std::nullopt;
        this->state_->reservedBytes += bytes;
        return Reservation(this->state_, bytes);
    }

    std::uint64_t reservedBytes() const {
        const std::lock_guard<std::mutex> lock(this->state_->mutex);
        return this->state_->reservedBytes;
    }
    std::uint64_t maximumBytes() const noexcept { return this->state_->maximumBytes; }

private:
    std::shared_ptr<State> state_;
};

} // namespace aowis::dvmdb::http
