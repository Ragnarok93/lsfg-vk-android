#include "adreno_source_protection.hpp"

#include <cassert>

int main() {
    AdrenoSourceProtectionController state;

    state.enterProtection(AdrenoSourceProtectionBackoffReason::AdmissionRejected);
    assert(state.protectedSourceOnly());

    // Four clean source-only frames are not enough to trigger another
    // reprime. The source cadence tracker itself requires six coherent clean
    // samples before it may promote a naturally slower baseline.
    for (uint32_t i = 0; i < 5; ++i) {
        state.observeProtectedSourceOnly();
        assert(!state.requestReprimeIfRecovered(
            true, false, true));
    }
    assert(state.telemetry().reprimeRequests == 0);

    // Even after the cadence hold, stale/unsafe synthetic capacity must not
    // cause a forced trial. Source-only recovery is allowed to relax the
    // predictor until it can actually prove one generated frame fits.
    state.observeProtectedSourceOnly();
    assert(!state.requestReprimeIfRecovered(
        true, false, true));
    assert(state.telemetry().reprimeRequests == 0);

    // Once both the real-source cadence and one-frame synthetic capacity have
    // recovered, exactly one reprime may be scheduled.
    assert(state.requestReprimeIfRecovered(
        true, true, true));
    assert(state.reprimePending());
    assert(!state.requestReprimeIfRecovered(
        true, true, true));
    state.onReprimeExecuted();
    assert(state.generationTrial());
    assert(state.telemetry().reprimesExecuted == 1);
    assert(state.telemetry().generationProbes == 1);

    // A failed trial returns to protected source-only and may not immediately
    // re-arm just because source cadence is healthy. Capacity evidence must
    // recover again first.
    state.enterProtection(AdrenoSourceProtectionBackoffReason::TrialFailed);
    assert(state.protectedSourceOnly());
    assert(state.telemetry().probeFailures == 1);
    for (uint32_t i = 0; i < 8; ++i) {
        state.observeProtectedSourceOnly();
        assert(!state.requestReprimeIfRecovered(
            true, false, true));
    }
    assert(state.telemetry().reprimeRequests == 1);

    return 0;
}
