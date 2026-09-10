#include "fixed_frame_governor.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

static std::size_t runStable(FixedFrameGovernor& governor,
        std::chrono::nanoseconds interval, int frames) {
    std::size_t out = 0;
    for (int i = 0; i < frames; ++i)
        out = governor.plan(interval);
    return out;
}

int main() {
    {
        FixedFrameGovernor governor;
        governor.configure(false, 2, 0);
        assert(governor.plan(16ms) == 1);
        governor.configure(false, 3, 0);
        assert(governor.plan(16ms) == 2);
        governor.configure(false, 4, 0);
        assert(governor.plan(16ms) == 3);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 2, 0);
        assert(governor.plan(0ns) == 1);
        assert(governor.plan(500ms) == 1);
        assert(runStable(governor, 16ms, 300) == 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        for (int i = 0; i < 500; ++i) {
            const auto out = governor.plan(16ms);
            assert(out >= 1 && out <= 3);
        }
        assert(governor.telemetry().costLimit == 3);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 120);
        const auto out = runStable(governor, 16'666'667ns, 400);
        assert(out == 1);
        assert(governor.telemetry().costLimit == 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        runStable(governor, 16ms, 200);
        assert(governor.plan(400ms) >= 1);
        assert(governor.telemetry().discontinuityReset);
        assert(governor.plan(16ms) >= 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        runStable(governor, 16ms, 100);
        governor.configure(true, 3, 0);
        const auto out = governor.plan(16ms);
        assert(out == 1);
        assert(out <= 2);
    }
}
