#include "accelerator_coordinator.hpp"
#include "android_diagnostics.hpp"
#include "qnn_htp_smoke_probe.hpp"

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

    if (!status_.npuSettingEnabled) {
        status_.selectionReason = "npu-setting-disabled";
        logSnapshot();
        return;
    }

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

    status_.executionEnabled = false;
    status_.selectedBackend = BackendKind::Vulkan;

    const bool qnnPhase2Qualified = status_.qnnProviderQualified
        && status_.qnnSystemProviderQualified
        && status_.qnnHtpAttributionQualified
        && status_.qnnSharedMemoryQualified
        && status_.qnnGraphExecutionQualified
        && status_.qnnNumericalSmokeQualified;

    if (qnnPhase2Qualified) {
        status_.healthState = AcceleratorHealthState::Supported;
        status_.directAhbInteropQualified = true;
        status_.selectionReason = "phase2-execution-disabled";
        status_.fallbackReason = "phase3-dynamic-warp-benchmark-required";
    } else if (status_.qnnProviderQualified && status_.qnnSystemProviderQualified) {
        status_.healthState = AcceleratorHealthState::Degraded;
        status_.directAhbInteropQualified = false;
        status_.selectionReason = "qnn-phase2-smoke-unqualified";
        if (status_.fallbackReason.empty())
            status_.fallbackReason = "qnn-phase2-smoke-unqualified";
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

    if (!status_.qnnProviderQualified || !status_.qnnSystemProviderQualified) {
        status_.fallbackReason = result.failureReason.empty()
            ? "qnn-provider-unqualified" : result.failureReason;
        logAccelerator(
            std::string("LSFG_ACCEL event=qnn-provider-rejected reason=") + status_.fallbackReason);
        return;
    }

    logAccelerator(
        std::string("LSFG_ACCEL event=qnn-provider-qualified")
        + " provider=" + logToken(status_.qnnProviderName)
        + " backend_id=" + std::to_string(status_.qnnBackendId)
        + " core_api=" + versionToken(status_.qnnCoreApiVersion)
        + " backend_api=" + versionToken(status_.qnnBackendApiVersion)
        + " system_api=" + versionToken(status_.qnnSystemApiVersion)
        + " htp_module=" + logToken(status_.qnnHtpLibraryPath)
        + " system_module=" + logToken(status_.qnnSystemLibraryPath));

    const QnnHtpSmokeResult smoke = probeQnnHtpGraphAndSharedMemory(qnnHtpHandle_);
    status_.qnnHtpAttributionQualified = smoke.htpAttributionQualified;
    status_.qnnSharedMemoryQualified = smoke.sharedMemoryQualified;
    status_.qnnGraphExecutionQualified = smoke.graphExecutionQualified;
    status_.qnnNumericalSmokeQualified = smoke.numericalSmokeQualified;
    status_.qnnBackendBuildId = smoke.backendBuildId;

    const bool qualified = smoke.htpAttributionQualified
        && smoke.sharedMemoryQualified
        && smoke.graphExecutionQualified
        && smoke.numericalSmokeQualified;
    if (!qualified)
        status_.fallbackReason = smoke.failureReason.empty()
            ? "qnn-phase2-smoke-unqualified" : smoke.failureReason;

    logAccelerator(
        std::string("LSFG_ACCEL event=qnn-phase2-smoke")
        + " htp_attribution=" + (smoke.htpAttributionQualified ? "1" : "0")
        + " ahb_shared=" + (smoke.sharedMemoryQualified ? "1" : "0")
        + " graph_execute=" + (smoke.graphExecutionQualified ? "1" : "0")
        + " numerical=" + (smoke.numericalSmokeQualified ? "1" : "0")
        + " backend_build=" + logToken(smoke.backendBuildId)
        + " result=" + (qualified ? "qualified" : "rejected")
        + " reason=" + (smoke.failureReason.empty() ? "none" : smoke.failureReason));
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
        + " qnn_htp_attribution_qualified=" + (status_.qnnHtpAttributionQualified ? "1" : "0")
        + " qnn_shared_memory_qualified=" + (status_.qnnSharedMemoryQualified ? "1" : "0")
        + " qnn_graph_execution_qualified=" + (status_.qnnGraphExecutionQualified ? "1" : "0")
        + " qnn_numerical_smoke_qualified=" + (status_.qnnNumericalSmokeQualified ? "1" : "0")
        + " direct_ahb_interop_qualified=" + (status_.directAhbInteropQualified ? "1" : "0")
        + " qnn_provider=" + logToken(status_.qnnProviderName)
        + " qnn_backend_id=" + std::to_string(status_.qnnBackendId)
        + " qnn_core_api=" + versionToken(status_.qnnCoreApiVersion)
        + " qnn_backend_api=" + versionToken(status_.qnnBackendApiVersion)
        + " qnn_system_api=" + versionToken(status_.qnnSystemApiVersion)
        + " qnn_htp_module=" + logToken(status_.qnnHtpLibraryPath)
        + " qnn_system_module=" + logToken(status_.qnnSystemLibraryPath)
        + " qnn_backend_build=" + logToken(status_.qnnBackendBuildId)
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
