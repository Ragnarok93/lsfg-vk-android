#pragma once

#include "adaptive_scheduler.hpp"
#include <algorithm>
#include <limits>

// Presentation latency is separate from source/admission credit. Shift both
// endpoints of a batch together, including its source. Contract an overlapping
// batch window when cadence speeds up instead of accumulating presentation debt.
class SourcePresentationTimeline {
public:
    SourceTimelineSample schedule(const SourceTimelineSample& source, uint64_t delayNs) {
        if (!source.valid) {
            reset();
            return {};
        }
        if (delayNs == 0) return source;
        const auto maximum = std::numeric_limits<uint64_t>::max();
        if (source.sourceDesiredTimeNs <= source.previousSourceDesiredTimeNs
                || source.sourceDesiredTimeNs > maximum - delayNs)
            return {};
        auto result = source;
        result.previousSourceDesiredTimeNs = std::max(
            source.previousSourceDesiredTimeNs + delayNs, lastSourceDesiredTimeNs_);
        result.sourceDesiredTimeNs = source.sourceDesiredTimeNs + delayNs;
        if (result.sourceDesiredTimeNs <= result.previousSourceDesiredTimeNs) {
            // No interpolation window remains. Keep this source ordered and
            // let the caller run a source-only history cycle.
            if (lastSourceDesiredTimeNs_ == maximum) return {};
            result.sourceDesiredTimeNs = lastSourceDesiredTimeNs_ + 1;
            result.valid = false;
        }
        lastSourceDesiredTimeNs_ = result.sourceDesiredTimeNs;
        return result;
    }

    void reset() { lastSourceDesiredTimeNs_ = 0; }

private:
    uint64_t lastSourceDesiredTimeNs_{0};
};
