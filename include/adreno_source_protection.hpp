#pragma once

#include <cstddef>
#include <cstdint>

enum class AdrenoSourceProtectionPhase {
    NormalGeneration,
    ProtectedSourceOnly,
    ReprimePending,
    GenerationTrial,
};

enum class AdrenoSourceProtectionBackoffReason {
    None,
    FixedCadence,
    AdmissionRejected,
    ComputeOverBudget,
    TrialFailed,
};

struct AdrenoSourceProtectionTelemetry {
    AdrenoSourceProtectionPhase phase{AdrenoSourceProtectionPhase::NormalGeneration};
    AdrenoSourceProtectionBackoffReason backoffReason{AdrenoSourceProtectionBackoffReason::None};
    uint64_t sourceOnlyRecoveryFrames{};
    uint64_t reprimeRequests{};
    uint64_t reprimesExecuted{};
    uint64_t generationProbes{};
    uint64_t probeSuccesses{};
    uint64_t probeFailures{};
    uint64_t sourceOnlyBypasses{};
    uint32_t recoveryFrames{};
};

class AdrenoSourceProtectionController {
public:
    // SourceProtectionBudgetTracker requires six coherent source-only samples
    // before it may promote a naturally slower baseline. Never schedule a
    // reprime earlier than that promotion window or the host-fence warmup will
    // interrupt the very evidence needed to establish recovery.
    static constexpr uint32_t kRecoveryFramesBeforeProbe = 6;

    void reset() {
        telemetry_ = {};
    }

    void enterProtection(AdrenoSourceProtectionBackoffReason reason) {
        if (telemetry_.phase == AdrenoSourceProtectionPhase::GenerationTrial) {
            telemetry_.probeFailures++;
        }
        telemetry_.phase = AdrenoSourceProtectionPhase::ProtectedSourceOnly;
        telemetry_.backoffReason = reason;
        telemetry_.recoveryFrames = 0;
    }

    void observeProtectedSourceOnly() {
        if (telemetry_.phase != AdrenoSourceProtectionPhase::ProtectedSourceOnly)
            return;
        telemetry_.sourceOnlyRecoveryFrames++;
        telemetry_.sourceOnlyBypasses++;
        if (telemetry_.recoveryFrames < UINT32_MAX)
            telemetry_.recoveryFrames++;
    }

    [[nodiscard]] bool requestReprimeIfRecovered(
            bool sourceRecoveryValid,
            bool syntheticCapacityRecovered,
            bool generationDemand) {
        // A reprime is itself source-visible work. Require independent proof
        // that the real-source cadence is healthy and that one synthetic batch
        // fits the source-owned interval. Cold-start bootstrap is represented
        // by syntheticCapacityRecovered=true only while no cost estimate exists.
        if (telemetry_.phase != AdrenoSourceProtectionPhase::ProtectedSourceOnly
                || !sourceRecoveryValid
                || !syntheticCapacityRecovered
                || !generationDemand
                || telemetry_.recoveryFrames < kRecoveryFramesBeforeProbe) {
            return false;
        }
        telemetry_.phase = AdrenoSourceProtectionPhase::ReprimePending;
        telemetry_.reprimeRequests++;
        return true;
    }

    void onReprimeExecuted() {
        if (telemetry_.phase != AdrenoSourceProtectionPhase::ReprimePending)
            return;
        telemetry_.phase = AdrenoSourceProtectionPhase::GenerationTrial;
        telemetry_.reprimesExecuted++;
        telemetry_.generationProbes++;
    }

    void onTrialSucceeded() {
        if (telemetry_.phase != AdrenoSourceProtectionPhase::GenerationTrial)
            return;
        telemetry_.phase = AdrenoSourceProtectionPhase::NormalGeneration;
        telemetry_.backoffReason = AdrenoSourceProtectionBackoffReason::None;
        telemetry_.probeSuccesses++;
        telemetry_.recoveryFrames = 0;
    }

    [[nodiscard]] bool protectedSourceOnly() const {
        return telemetry_.phase == AdrenoSourceProtectionPhase::ProtectedSourceOnly;
    }

    [[nodiscard]] bool reprimePending() const {
        return telemetry_.phase == AdrenoSourceProtectionPhase::ReprimePending;
    }

    [[nodiscard]] bool generationTrial() const {
        return telemetry_.phase == AdrenoSourceProtectionPhase::GenerationTrial;
    }

    [[nodiscard]] const AdrenoSourceProtectionTelemetry& telemetry() const {
        return telemetry_;
    }

    static constexpr const char* phaseName(AdrenoSourceProtectionPhase phase) {
        switch (phase) {
        case AdrenoSourceProtectionPhase::NormalGeneration: return "normal-generation";
        case AdrenoSourceProtectionPhase::ProtectedSourceOnly: return "protected-source-only";
        case AdrenoSourceProtectionPhase::ReprimePending: return "reprime-pending";
        case AdrenoSourceProtectionPhase::GenerationTrial: return "generation-trial";
        }
        return "normal-generation";
    }

    static constexpr const char* backoffReasonName(
            AdrenoSourceProtectionBackoffReason reason) {
        switch (reason) {
        case AdrenoSourceProtectionBackoffReason::None: return "none";
        case AdrenoSourceProtectionBackoffReason::FixedCadence: return "fixed-cadence";
        case AdrenoSourceProtectionBackoffReason::AdmissionRejected: return "admission-rejected";
        case AdrenoSourceProtectionBackoffReason::ComputeOverBudget: return "compute-over-budget";
        case AdrenoSourceProtectionBackoffReason::TrialFailed: return "trial-failed";
        }
        return "none";
    }

private:
    AdrenoSourceProtectionTelemetry telemetry_{};
};
