#include "accelerator_coordinator.hpp"
#include "android_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#ifdef __ANDROID__
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
    LSFG::AndroidDiagnostics::logNativeDiagnostic(message);
}

std::string logToken(std::string value) {
    if (value.empty()) return "none";
    std::replace_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }, '_');
    return value;
}

std::string versionToken(const QnnVersion& version) {
    if (!version.valid()) return "none";
    return std::to_string(version.major) + "." + std::to_string(version.minor)
        + "." + std::to_string(version.patch);
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
    logAccelerator("LSFG_ACCEL event=probe-begin phase=2");

    if (status_.requestedBackend == BackendOverride::Snpe) {
        probeSnpeRuntime();
    } else {
        probeQnnRuntime();
        if ((!status_.qnnProviderQualified || !status_.qnnSystemProviderQualified)
                && status_.requestedBackend == BackendOverride::Auto) {
            probeSnpeRuntime();
        }
    }

    // Phase 2 invariant: provider qualification is still informational. Direct
    // AHB -> QNN memory registration and actual HTP graph execution have not yet
    // been proven, so no runtime discovery result is allowed to steal execution
    // from the validated Vulkan path.
    status_.executionEnabled = false;
    status_.selectedBackend = BackendKind::Vulkan;
    status_.directAhbInteropQualified = false;

    if (status_.qnnProviderQualified && status_.qnnSystemProviderQualified) {
        status_.healthState = AcceleratorHealthState::Supported;
        status_.selectionReason = "phase2-execution-disabled";
        status_.fallbackReason = "direct-ahb-qnn-memory-registration-unproven";
    } else if (status_.snpeRuntimeFound) {
        status_.healthState = AcceleratorHealthState::Unprobed;
        status_.selectionReason = "snpe-phase2-unqualified";
        status_.fallbackReason = "snpe-provider-qualification-not-implemented";
    } else {
        status_.healthState = AcceleratorHealthState::Unprobed;
        if (status_.fallbackReason.empty())
            status_.fallbackReason = "accelerator-runtime-unavailable";
        status_.selectionReason = status_.fallbackReason;
    }

    logAccelerator(
        std::string("LSFG_ACCEL event=vulkan-fallback reason=") + status_.fallbackReason);
    logSnapshot();

    // Metadata inspection does not need accelerator runtimes to remain resident.
    closeRuntimeHandles();
}

void AcceleratorCoordinator::endSession() noexcept {
    closeRuntimeHandles();
    status_ = AcceleratorStatus{};
}

void AcceleratorCoordinator::probeQnnRuntime() {
    const QnnRuntimeProbeResult result =
        probeQnnRuntimeMetadata(qnnSystemHandle_, qnnHtpHandle_);

    status_.qnnRuntimeFound = result.systemLibraryLoaded && result.htpLibraryLoaded;
    status_.qnnProviderQualified = result.qnnProviderQualified;
    status_.qnnSystemProviderQualified = result.qnnSystemProviderQualified;
    status_.qnnBackendId = result.backendId;
    status_.qnnProviderName = result.providerName;
    status_.qnnSystemProviderName = result.systemProviderName;
    status_.qnnCoreApiVersion = result.coreApiVersion;
    status_.qnnBackendApiVersion = result.backendApiVersion;
    status_.qnnSystemApiVersion = result.systemApiVersion;
    status_.qnnHtpLibraryPath = result.htpLibraryPath;
    status_.qnnSystemLibraryPath = result.systemLibraryPath;

    if (status_.qnnProviderQualified && status_.qnnSystemProviderQualified) {
        logAccelerator(
            std::string("LSFG_ACCEL event=qnn-provider-qualified")
            + " provider=" + logToken(status_.qnnProviderName)
            + " backend_id=" + std::to_string(status_.qnnBackendId)
            + " core_api=" + versionToken(status_.qnnCoreApiVersion)
            + " backend_api=" + versionToken(status_.qnnBackendApiVersion)
            + " system_api=" + versionToken(status_.qnnSystemApiVersion)
            + " htp_module=" + logToken(status_.qnnHtpLibraryPath)
            + " system_module=" + logToken(status_.qnnSystemLibraryPath));
        return;
    }

    status_.fallbackReason = result.failureReason.empty()
        ? "qnn-provider-unqualified" : result.failureReason;
    logAccelerator(
        std::string("LSFG_ACCEL event=qnn-provider-rejected reason=") + status_.fallbackReason);
}

void AcceleratorCoordinator::probeSnpeRuntime() {
    snpeHandle_ = openOptionalRuntime("libSNPE.so");
    status_.snpeRuntimeFound = snpeHandle_ != nullptr;
    if (status_.snpeRuntimeFound)
        logAccelerator("LSFG_ACCEL event=snpe-runtime-found phase2_provider_qualified=0");
}

void AcceleratorCoordinator::closeRuntimeHandles() noexcept {
    closeOptionalRuntime(qnnSystemHandle_);
    closeOptionalRuntime(qnnHtpHandle_);
    closeOptionalRuntime(snpeHandle_);
}

void AcceleratorCoordinator::logSnapshot() const {
    logAccelerator(
        std::string("LSFG_ACCEL npu_setting_enabled=") + (status_.npuSettingEnabled ? "1" : "0")
        + " accelerator_module_version=phase2"
        + " qnn_runtime_found=" + (status_.qnnRuntimeFound ? "1" : "0")
        + " qnn_provider_qualified=" + (status_.qnnProviderQualified ? "1" : "0")
        + " qnn_system_provider_qualified=" + (status_.qnnSystemProviderQualified ? "1" : "0")
        + " direct_ahb_interop_qualified=" + (status_.directAhbInteropQualified ? "1" : "0")
        + " qnn_provider=" + logToken(status_.qnnProviderName)
        + " qnn_backend_id=" + std::to_string(status_.qnnBackendId)
        + " qnn_core_api=" + versionToken(status_.qnnCoreApiVersion)
        + " qnn_backend_api=" + versionToken(status_.qnnBackendApiVersion)
        + " qnn_system_api=" + versionToken(status_.qnnSystemApiVersion)
        + " qnn_htp_module=" + logToken(status_.qnnHtpLibraryPath)
        + " qnn_system_module=" + logToken(status_.qnnSystemLibraryPath)
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
