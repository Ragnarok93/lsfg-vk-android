#include "adaptive_frame_scheduler.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

constexpr uint64_t frameNs(double fps) {
    return static_cast<uint64_t>(1'000'000'000.0 / fps);
}

std::vector<size_t> run(AdaptiveFrameScheduler& scheduler, double sourceFps,
        size_t frames) {
    std::vector<size_t> result;
    uint64_t now = 1'000'000'000ULL;
    for (size_t i = 0; i < frames; ++i) {
        result.push_back(scheduler.generatedFrames(now));
        now += frameNs(sourceFps);
    }
    return result;
}

size_t sum(const std::vector<size_t>& values) {
    size_t result = 0;
    for (const auto value : values) result += value;
    return result;
}

} // namespace

int main() {
    {
        // Defect: a 30 FPS source with a 30 FPS final cap was provisioned as
        // multiplier 4 and unconditionally emitted three generated frames.
        AdaptiveFrameScheduler scheduler(true, 30, 3);
        const auto generated = run(scheduler, 30.0, 120);
        assert(sum(generated) == 0);
    }

    {
        // A fractional target must distribute work instead of rounding the
        // entire session up to the next multiplier.
        AdaptiveFrameScheduler scheduler(true, 45, 3);
        const auto generated = run(scheduler, 30.0, 62);
        assert(generated[0] == 0); // warm-up never exposes an invalid output
        const size_t generatedAfterWarmup = sum(generated);
        assert(generatedAfterWarmup >= 29 && generatedAfterWarmup <= 31);
    }

    {
        AdaptiveFrameScheduler scheduler(true, 120, 3);
        const auto generated = run(scheduler, 30.0, 10);
        assert(generated[0] == 0);
        for (size_t i = 1; i < generated.size(); ++i) assert(generated[i] == 3);
    }

    {
        // A menu/config stall must not accumulate a burst of stale frames.
        AdaptiveFrameScheduler scheduler(true, 60, 3);
        uint64_t now = 1'000'000'000ULL;
        assert(scheduler.generatedFrames(now) == 0);
        now += frameNs(30.0);
        assert(scheduler.generatedFrames(now) == 1);
        now += 2'000'000'000ULL;
        assert(scheduler.generatedFrames(now) == 0);
    }

    {
        assert((selectGeneratedFrameIndices(3, 0) == std::vector<size_t>{}));
        assert((selectGeneratedFrameIndices(3, 1) == std::vector<size_t>{1}));
        assert((selectGeneratedFrameIndices(3, 2) == std::vector<size_t>{0, 2}));
        assert((selectGeneratedFrameIndices(3, 3) == std::vector<size_t>{0, 1, 2}));
    }
}
