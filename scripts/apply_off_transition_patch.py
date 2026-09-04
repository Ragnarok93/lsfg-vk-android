#!/usr/bin/env python3
from pathlib import Path

path = Path("src/hooks.cpp")
text = path.read_text()

old_runtime = '''        if (state.pendingRequiresRecreate) {
            return state.wsiRecreateGate.requestIfNeeded(true)
                ? PendingConfigAction::Recreate
                : PendingConfigAction::None;
        }
'''
new_runtime = '''        if (state.pendingRequiresRecreate) {
            const bool nextNeedsFrameGeneration =
                state.pendingConfig->enable && state.pendingConfig->multiplier > 1;
            const auto action = state.wsiRecreateGate.requestIfNeeded(true)
                ? PendingConfigAction::Recreate
                : PendingConfigAction::None;
            if (action == PendingConfigAction::Recreate && !nextNeedsFrameGeneration) {
                // OFF is committed before WSI restoration so no stale enabled
                // controller or source-pacer policy can survive the accepted
                // disengagement request while the application recreates WSI.
                Config::activeConf = *state.pendingConfig;
                state.appliedConf = Config::activeConf;
                state.pendingConfig.reset();
                state.pendingRequiresRecreate = false;
                state.configPending.store(false, std::memory_order_release);
                std::cerr << "lsfg-vk: runtime stage=frame-generation-disabled-before-wsi-restore\\n";
            }
            return action;
        }
'''

old_present = '''        if (pendingAction == PendingConfigAction::Recreate) {
            // Transition frames are native. Returning OUT_OF_DATE after the
            // native present asks the application to rebuild WSI from its own
            // untouched create parameters; sustained Off then has no LSFG WSI.
            Layer::ovkQueuePresentKHR(queue, pPresentInfo);
            std::cerr << "lsfg-vk: runtime stage=wsi-restore-requested reason=config-transition\\n";
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
'''
new_present = '''        if (pendingAction == PendingConfigAction::Recreate) {
            // Transition frames are native. Retire every LSFG-owned object for
            // the old swapchain immediately after that source present; the
            // replacement WSI is then rebuilt from the application's untouched
            // create parameters, and sustained OFF has no context/AHB/pacer work.
            Layer::ovkQueuePresentKHR(queue, pPresentInfo);
            eraseSwapchainState(*pPresentInfo->pSwapchains);
            std::cerr << "lsfg-vk: runtime stage=wsi-restore-requested reason=config-transition\\n";
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
'''

runtime_count = text.count(old_runtime)
present_count = text.count(old_present)
if runtime_count != 1:
    raise SystemExit(f"runtime replacement count={runtime_count}")
if present_count != 1:
    raise SystemExit(f"present replacement count={present_count}")

path.write_text(text.replace(old_runtime, new_runtime).replace(old_present, new_present))
