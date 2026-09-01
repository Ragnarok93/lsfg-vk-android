#include "source_frame_pacer.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;

namespace {

void expectNear(std::chrono::nanoseconds actual,
        std::chrono::nanoseconds expected,
        std::chrono::microseconds tolerance) {
    const auto delta = actual > expected ? actual - expected : expected - actual;
    assert(delta <= tolerance);
}

void capsThirtyFpsWithoutDelayingFirstSourceFrame() {
    SourceFramePacer pacer;
    const auto t0 = SourceFramePacer::Clock::time_point{};
    pacer.configure(30);

    assert(pacer.delayUntilNext(t0) == 0ns);
    expectNear(pacer.delayUntilNext(t0 + 5ms), 28'333'333ns, 2us);
}

void disablingAndRetargetingResetTheCadence() {
    SourceFramePacer pacer;
    const auto t0 = SourceFramePacer::Clock::time_point{};
    pacer.configure(30);
    assert(pacer.delayUntilNext(t0) == 0ns);

    pacer.configure(0);
    assert(pacer.delayUntilNext(t0 + 1ms) == 0ns);

    pacer.configure(60);
    assert(pacer.delayUntilNext(t0 + 2ms) == 0ns);
    expectNear(pacer.delayUntilNext(t0 + 3ms), 15'666'667ns, 2us);
}

void aLateSourceFrameResynchronizesInsteadOfBursting() {
    SourceFramePacer pacer;
    const auto t0 = SourceFramePacer::Clock::time_point{};
    pacer.configure(30);
    assert(pacer.delayUntilNext(t0) == 0ns);

    assert(pacer.delayUntilNext(t0 + 100ms) == 0ns);
    expectNear(pacer.delayUntilNext(t0 + 105ms), 28'333'333ns, 2us);
}

} // namespace

int main() {
    capsThirtyFpsWithoutDelayingFirstSourceFrame();
    disablingAndRetargetingResetTheCadence();
    aLateSourceFrameResynchronizesInsteadOfBursting();
    return 0;
}
