#include "adreno_source_protection.hpp"

#include <cassert>

int main() {
    AdrenoSourceProtectionController state;

    // Protection stays genuinely source-only across consecutive frames.
    state.enterProtection(AdrenoSourceProtectionBackoffReason::AdmissionRejected);
    assert(state.protectedSourceOnly());
    for (uint32_t i = 0; i < 8; ++i) {
        const bool reprime = state.requestReprimeIfRecovered(
            false, true);
        assert(!reprime);
        state.observeProtectedSourceOnly();
        assert(state.protectedSourceOnly());
        assert(!state.reprimePending());
    }
    assert(state.telemetry().reprimeRequests == 0);
    assert(state.telemetry().sourceOnlyRecoveryFrames == 8);
    assert(!state.requestReprimeIfRecovered(true, false));

    // Sustained clean-source recovery plus current generation demand authorizes
    // one reprime. Old predictor capacity is intentionally not an input: the
    // following minimum-cost trial is what refreshes that evidence.
    assert(state.requestReprimeIfRecovered(true, true));
    assert(state.reprimePending());
    assert(!state.requestReprimeIfRecovered(true, true));
    state.onReprimeExecuted();
    assert(state.generationTrial());
    assert(state.telemetry().reprimesExecuted == 1);
    assert(state.telemetry().generationProbes == 1);

    // A failed trial returns to protected mode without scheduling another reprime.
    state.enterProtection(AdrenoSourceProtectionBackoffReason::TrialFailed);
    assert(state.protectedSourceOnly());
    assert(state.telemetry().probeFailures == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        state.observeProtectedSourceOnly();
        assert(!state.requestReprimeIfRecovered(true, true));
    }
    assert(state.telemetry().reprimeRequests == 1);

    state.observeProtectedSourceOnly();
    assert(state.requestReprimeIfRecovered(true, true));
    state.onReprimeExecuted();

    // The generation trial exists specifically to refresh stale synthetic-cost
    // evidence. A stale predictor rejection must not erase the one-frame probe
    // after source-only recovery and a deliberate reprime.
    assert(state.minimumGenerationTrial(1, 0) == 1);
    assert(state.minimumGenerationTrial(3, 0) == 1);
    assert(state.minimumGenerationTrial(0, 0) == 0);

    state.onTrialSucceeded();
    assert(!state.protectedSourceOnly());
    assert(state.telemetry().probeSuccesses == 1);
    assert(state.telemetry().reprimesExecuted == 2);

    return 0;
}
