#include "accelerator_coordinator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#endif

namespace LSFG::Accelerator {
namespace {

bool envEnabled(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return false;

    std::string value(raw);
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

BackendOverride requestedBackendFromEnvironment() {
    const char* raw = std::getenv("LSFG_ACCEL_BACKEND");
    if (raw == nullptr || *raw == '\0') return BackendOverride::Auto;

    std::string value(raw);
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "vulkan") return BackendOverride::Vulkan;
    if (value == "qnn") return BackendOverride::Qnn;
    if (value == "snpe") return BackendOverride::Snpe;
    return BackendOverride::Auto;
}

void logAccelerator(const std::string& message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSFG", "%s", message.c_str());
#endif
    std::cerr << message << '\n';
}

#ifdef __ANDROID__
void* openOptionalRuntime(const char* library) {
    // Deliberately no link-time Qualcomm dependency: failure is ordinary and
    // always falls through to the preserved Vulkan implementation.
    return dlopen(library, RTLD_NOW | RTLD_LOCAL);
}

void closeOptionalRuntime(void*& handle) noexcept {
    if (handle != nullptr) {
        dlclose(handle);
        handle = nullptr;
    }
}
#else
void* openOptionalRuntime(const char*) { return nullptr; }
void closeOptionalRuntime(void*& handle) noexcept { handle = nullptr; }
#endif

} // namespace

AcceleratorCoordinator& AcceleratorCoordinator::instance() noexcept {
    static AcceleratorCoordinator coordinator;
    return coordinator;
}

AcceleratorCoordinator::~AcceleratorCoordinator() {
    closeRuntimeHandles();
}

void AcceleratorCoordinator::beginEligibleSession() {
    closeRuntimeHandles();
    status_ = AcceleratorStatus{};
    status_.selectedBackend = BackendKind::Vulkan;
    status_.npuSettingEnabled = envEnabled("LSFG_NPU_ACCELERATION");
    status_.requestedBackend = requestedBackendFromEnvironment();

    // The production opt-in is authoritative. Debug backend overrides never
    // bypass the user's NPU permission and never force accelerator loading.
    if (!status_.npuSettingEnabled) {
        status_.selectionReason = "npu-setting-disabled";
        logSnapshot();
        return;
    }

    // Forced Vulkan is useful for A/B testing and intentionally avoids even a
    // Qualcomm runtime probe.
    if (status_.requestedBackend == BackendOverride::Vulkan) {
        status_.selectionReason = "forced-vulkan";
        logSnapshot();
        return;
    }

    status_.healthState = AcceleratorHealthState::Probing;
    logAccelerator("LSFG_ACCEL event=probe-begin");

    if (status_.requestedBackend == BackendOverride::Snpe) {
        probeSnpeRuntime();
    } else {
        probeQnnRuntime();
        if (!status_.qnnRuntimeFound && status_.requestedBackend == BackendOverride::Auto)
            probeSnpeRuntime();
    }

    // Phase 1 invariant: discovery is informational only. No QNN/SNPE graph is
    // created and Vulkan remains the unconditional execution backend.
    status_.executionEnabled = false;
    status_.selectedBackend = BackendKind::Vulkan;
    if (status_.qnnRuntimeFound || status_.snpeRuntimeFound) {
        status_.selectionReason = "phase1-execution-disabled";
        status_.fallbackReason = "phase1-execution-disabled";
    } else {
        status_.selectionReason = "accelerator-runtime-unavailable";
        status_.fallbackReason = "accelerator-runtime-unavailable";
    }
    status_.healthState = AcceleratorHealthState::Unprobed;

    logAccelerator(
        std::string("LSFG_ACCEL event=vulkan-fallback reason=") + status_.fallbackReason);
    logSnapshot();

    // Discovery must not retain accelerator resources in Phase 1. Later phases
    // may keep qualified runtimes resident only while an accelerator backend is
    // actually warm/active.
    closeRuntimeHandles();
}

void AcceleratorCoordinator::endSession() noexcept {
    closeRuntimeHandles();
    status_ = AcceleratorStatus{};
}

void AcceleratorCoordinator::probeQnnRuntime() {
    qnnSystemHandle_ = openOptionalRuntime("libQnnSystem.so");
    qnnHtpHandle_ = openOptionalRuntime("libQnnHtp.so");
    status_.qnnRuntimeFound = qnnSystemHandle_ != nullptr && qnnHtpHandle_ != nullptr;

    if (!status_.qnnRuntimeFound) {
        closeOptionalRuntime(qnnSystemHandle_);
        closeOptionalRuntime(qnnHtpHandle_);
        return;
    }

    logAccelerator("LSFG_ACCEL event=qnn-runtime-found");
}

void AcceleratorCoordinator::probeSnpeRuntime() {
    snpeHandle_ = openOptionalRuntime("libSNPE.so");
    status_.snpeRuntimeFound = snpeHandle_ != nullptr;
    if (status_.snpeRuntimeFound)
        logAccelerator("LSFG_ACCEL event=snpe-runtime-found");
}

void AcceleratorCoordinator::closeRuntimeHandles() noexcept {
    closeOptionalRuntime(qnnSystemHandle_);
    closeOptionalRuntime(qnnHtpHandle_);
    closeOptionalRuntime(snpeHandle_);
}

void AcceleratorCoordinator::logSnapshot() const {
    logAccelerator(
        std::string("LSFG_ACCEL npu_setting_enabled=") + (status_.npuSettingEnabled ? "1" : "0")
        + " accelerator_module_version=phase1"
        + " qnn_runtime_found=" + (status_.qnnRuntimeFound ? "1" : "0")
        + " snpe_runtime_found=" + (status_.snpeRuntimeFound ? "1" : "0")
        + " requested_backend=" + backendOverrideName(status_.requestedBackend)
        + " selected_backend=" + backendKindName(status_.selectedBackend)
        + " selection_reason=" + status_.selectionReason
        + " health_state=" + acceleratorHealthStateName(status_.healthState)
        + " fallback_reason=" + (status_.fallbackReason.empty() ? "none" : status_.fallbackReason));
}

const char* acceleratorHealthStateName(AcceleratorHealthState state) noexcept {
    switch (state) {
        case AcceleratorHealthState::Unprobed: return "unprobed";
        case AcceleratorHealthState::Probing: return "probing";
        case AcceleratorHealthState::Supported: return "supported";
        case AcceleratorHealthState::Compiling: return "compiling";
        case AcceleratorHealthState::Warming: return "warming";
        case AcceleratorHealthState::Ready: return "ready";
        case AcceleratorHealthState::Active: return "active";
        case AcceleratorHealthState::Degraded: return "degraded";
        case AcceleratorHealthState::Quarantined: return "quarantined";
    }
    return "unprobed";
}

const char* backendOverrideName(BackendOverride backend) noexcept {
    switch (backend) {
        case BackendOverride::Auto: return "auto";
        case BackendOverride::Vulkan: return "vulkan";
        case BackendOverride::Qnn: return "qnn";
        case BackendOverride::Snpe: return "snpe";
    }
    return "auto";
}

} // namespace LSFG::Accelerator
