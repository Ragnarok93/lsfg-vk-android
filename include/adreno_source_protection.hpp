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
    static constexpr uint32_t kRecoveryFramesBeforeProbe = 4;

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
            bool generationDemand) {
        // Recovery is established by clean source-only cadence plus current
        // demand. Do not require the old synthetic-cost predictor to approve
        // the reprime: the following one-frame GenerationTrial exists
        // specifically to refresh stale generated-work cost evidence.
        if (telemetry_.phase != AdrenoSourceProtectionPhase::ProtectedSourceOnly
                || !sourceRecoveryValid
                || !generationDemand
                || telemetry_.recoveryFrames < kRecoveryFramesBeforeProbe) {
            return false;
        }
        telemetry_.phase = AdrenoSourceProtectionPhase::ReprimePending;
        telemetry_.reprimeRequests++;
        return true;
    }

    [[nodiscard]] std::size_t minimumGenerationTrial(
            std::size_t plannedGeneration,
            std::size_t admittedGeneration) const {
        if (telemetry_.phase != AdrenoSourceProtectionPhase::GenerationTrial
                || plannedGeneration == 0
                || admittedGeneration > 0) {
            return admittedGeneration;
        }

        // A stale predictor may reject the first candidate after source-only
        // recovery. Permit exactly one synthetic frame so the trial can
        // measure current blocking cost. The caller still requires a positive
        // real-source budget before applying this floor.
        return 1;
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
