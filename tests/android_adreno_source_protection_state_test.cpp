#include "adreno_source_protection.hpp"

#include <cassert>

int main() {
    AdrenoSourceProtectionController state;

    // Protection stays genuinely source-only across consecutive frames.
    state.enterProtection(AdrenoSourceProtectionBackoffReason::AdmissionRejected);
    assert(state.protectedSourceOnly());
    for (uint32_t i = 0; i < 8; ++i) {
        const bool reprime = state.requestReprimeIfRecovered(false);
        assert(!reprime);
        state.observeProtectedSourceOnly();
        assert(state.protectedSourceOnly());
        assert(!state.reprimePending());
    }
    assert(state.telemetry().reprimeRequests == 0);
    assert(state.telemetry().sourceOnlyRecoveryFrames == 8);

    // Sustained recovery evidence authorizes one reprime, then one low-cost trial.
    assert(state.requestReprimeIfRecovered(true));
    assert(state.reprimePending());
    assert(!state.requestReprimeIfRecovered(true));
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
        assert(!state.requestReprimeIfRecovered(true));
    }
    assert(state.telemetry().reprimeRequests == 1);

    state.observeProtectedSourceOnly();
    assert(state.requestReprimeIfRecovered(true));
    state.onReprimeExecuted();
    state.onTrialSucceeded();
    assert(!state.protectedSourceOnly());
    assert(state.telemetry().probeSuccesses == 1);
    assert(state.telemetry().reprimesExecuted == 2);

    return 0;
}
