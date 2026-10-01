#include "adaptive_flow_controller.hpp"
#include "adaptive_flow_handoff.hpp"
#include "swapchain_policy.hpp"

#include <cassert>
#include <cmath>
#include <limits>

enum class PresentMode : int { Fifo, Mailbox, Immediate };

static lsfg::handoff::Identity flowIdentity() {
    return lsfg::handoff::Identity{
        .enabled = true,
        .targeted = true,
        .adaptiveFramegen = true,
        .adaptiveFlow = true,
        .performance = false,
        .hdr = false,
        .multiplier = 2,
        .targetFps = 60,
        .width = 2400,
        .height = 1080,
        .dll = "Lossless.dll",
        .preset = "quality",
    };
}

static void testPresentModeSelectionAndReuse() {
    using namespace lsfg::wsi;
    constexpr auto fifo = PresentMode::Fifo;
    constexpr auto mailbox = PresentMode::Mailbox;
    constexpr auto immediate = PresentMode::Immediate;

    assert(modeRequestForCreate(false, fifo, mailbox, fifo) == mailbox);
    assert(modeRequestForCreate(true, fifo, mailbox, fifo) == mailbox);
    assert(modeRequestForCreate(true, fifo, fifo, mailbox) == mailbox);

    assert(applyResidentFifoBackend(fifo, true, fifo, fifo, mailbox, true) == mailbox);
    assert(applyResidentFifoBackend(fifo, true, fifo, fifo, mailbox, false) == fifo);
    assert(applyResidentFifoBackend(immediate, false, fifo, fifo, mailbox, true) == immediate);
    assert(!needsPhysicalRecreation(mailbox, mailbox));
    assert(needsPhysicalRecreation(mailbox, fifo));

    // Targeted Android uses MAILBOX for both logical FIFO and MAILBOX.
    // Repeated logical toggles must not rebuild the swapchain/context.
    auto logical = fifo;
    auto effective = mailbox;
    for (int i = 0; i < 1000; ++i) {
        logical = logical == fifo ? mailbox : fifo;
        const auto newEffective = applyResidentFifoBackend(
            logical, true, logical, fifo, mailbox, true);
        assert(!needsPhysicalRecreation(effective, newEffective));
        effective = newEffective;
    }

    // When FIFO cannot use the MAILBOX backend, a physical change is required
    // and explicit configuration must replace the application's old mode.
    const auto fifoFallback = applyResidentFifoBackend(
        fifo, true, fifo, fifo, mailbox, false);
    assert(needsPhysicalRecreation(mailbox, fifoFallback));
    assert(modeRequestForCreate(true, mailbox, fifo, mailbox) == fifo);

    // This is the old behavior: recreateExistingSwapchain always reused the
    // game's old mode, even after an explicit configuration change.
    const auto legacyRecreateMode = [](bool hasOldSwapchain,
            PresentMode configuredMode, PresentMode gameMode) {
        return hasOldSwapchain ? gameMode : configuredMode;
    };
    const auto legacyMode = legacyRecreateMode(true, mailbox, fifo);
    assert(legacyMode != mailbox);
}

static void testStableAdaptiveFlowHandoff() {
    auto previous = flowIdentity();
    auto next = flowIdentity();
    const auto candidate = lsfg::handoff::selectStableScale(
        true, previous, next, 0.85F, 0.85F, false, 0);
    assert(candidate && std::fabs(*candidate - 0.85F) < 0.0001F);
    assert(!lsfg::handoff::selectStableScale(
        false, previous, next, 0.85F, 0.85F, false, 0));

    auto changed = next;
    changed.targetFps = 90;
    assert(!lsfg::handoff::selectStableScale(
        true, previous, changed, 0.85F, 0.85F, false, 0));
    changed = next;
    changed.preset = "balanced";
    assert(!lsfg::handoff::selectStableScale(
        true, previous, changed, 0.85F, 0.85F, false, 0));
    changed = next;
    changed.width++;
    assert(!lsfg::handoff::selectStableScale(
        true, previous, changed, 0.85F, 0.85F, false, 0));
    changed = next;
    changed.performance = true;
    assert(!lsfg::handoff::selectStableScale(
        true, previous, changed, 0.85F, 0.85F, false, 0));
    changed = next;
    changed.adaptiveFlow = false;
    assert(!lsfg::handoff::selectStableScale(
        true, previous, changed, 0.85F, 0.85F, false, 0));

    assert(!lsfg::handoff::selectStableScale(
        true, previous, next, 0.85F, 0.85F, true, 0));
    assert(!lsfg::handoff::selectStableScale(
        true, previous, next, 0.85F, 0.85F, false, 1));
    assert(!lsfg::handoff::selectStableScale(
        true, previous, next, 0.80F, 0.85F, false, 0));
    assert(!lsfg::handoff::selectStableScale(
        true, previous, next, std::numeric_limits<float>::quiet_NaN(),
        0.85F, false, 0));

    // Old context recreation cold-started at the preset target.
    AdaptiveFlowController legacyContext;
    legacyContext.configure(true, AdaptiveFlowPreset::Quality);
    assert(std::fabs(legacyContext.currentScale() - 1.0F) < 0.0001F);

    const auto qualityStates =
        AdaptiveFlowController::statesForPreset(AdaptiveFlowPreset::Quality);
    const float learnedScale = qualityStates[3];
    AdaptiveFlowController previousController;
    previousController.configure(true, AdaptiveFlowPreset::Quality);
    assert(previousController.seedCurrentScale(learnedScale));
    assert(previousController.telemetry().stateIndex == 3);

    AdaptiveFlowController rebuiltController;
    rebuiltController.configure(true, AdaptiveFlowPreset::Quality);
    assert(rebuiltController.seedCurrentScale(*candidate));
    assert(std::fabs(rebuiltController.currentScale() - 0.85F) < 0.0001F);
    assert(rebuiltController.telemetry().stateIndex == 3);

    assert(!rebuiltController.seedCurrentScale(0.84F));
    assert(std::fabs(rebuiltController.currentScale() - 0.85F) < 0.0001F);
    rebuiltController.configure(true, AdaptiveFlowPreset::Balanced);
    assert(std::fabs(rebuiltController.currentScale() - 0.80F) < 0.0001F);
    assert(!rebuiltController.seedCurrentScale(learnedScale));
}

int main() {
    testPresentModeSelectionAndReuse();
    testStableAdaptiveFlowHandoff();
}
