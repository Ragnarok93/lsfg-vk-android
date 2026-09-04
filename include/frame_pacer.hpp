#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

/// Shared drift-corrected cadence engine used by the source limiter and the
/// LSFG output stream pacer. Roles remain separate at the call sites; only the
/// deadline/phase math is shared.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    void configure(uint32_t targetFps) noexcept {
        if (targetFps_ == targetFps)
            return;
        targetFps_ = targetFps;
        nextDeadline_.reset();
        phaseRemainder_ = 0;
        if (targetFps_ == 0) {
            basePeriodNs_ = 0;
            periodRemainder_ = 0;
            return;
        }
        basePeriodNs_ = 1'000'000'000ULL / static_cast<uint64_t>(targetFps_);
        periodRemainder_ = 1'000'000'000ULL % static_cast<uint64_t>(targetFps_);
    }

    [[nodiscard]] uint32_t targetFps() const noexcept { return targetFps_; }

    std::chrono::nanoseconds delayUntilNext(Clock::time_point now) noexcept {
        if (targetFps_ == 0) {
            nextDeadline_.reset();
            phaseRemainder_ = 0;
            return std::chrono::nanoseconds::zero();
        }

        if (!nextDeadline_.has_value()) {
            nextDeadline_ = now + nextPeriod();
            return std::chrono::nanoseconds::zero();
        }

        if (now >= *nextDeadline_) {
            // Preserve phase for ordinary small scheduling misses; discard a
            // stale timeline after a real stall so presentation never bursts to
            // catch up several old deadlines.
            const auto nominalPeriod = std::chrono::nanoseconds(basePeriodNs_ + 1);
            if (now - *nextDeadline_ > nominalPeriod * 2) {
                phaseRemainder_ = 0;
                *nextDeadline_ = now + nextPeriod();
                return std::chrono::nanoseconds::zero();
            }
            do {
                *nextDeadline_ += nextPeriod();
            } while (now >= *nextDeadline_);
            return std::chrono::nanoseconds::zero();
        }

        const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            *nextDeadline_ - now);
        *nextDeadline_ += nextPeriod();
        return delay;
    }

private:
    std::chrono::nanoseconds nextPeriod() noexcept {
        uint64_t periodNs = basePeriodNs_;
        phaseRemainder_ += periodRemainder_;
        if (phaseRemainder_ >= targetFps_) {
            phaseRemainder_ -= targetFps_;
            periodNs++;
        }
        return std::chrono::nanoseconds(periodNs);
    }

    uint32_t targetFps_{0};
    uint64_t basePeriodNs_{0};
    uint64_t periodRemainder_{0};
    uint64_t phaseRemainder_{0};
    std::optional<Clock::time_point> nextDeadline_{};
};
