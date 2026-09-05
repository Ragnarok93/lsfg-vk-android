#include "qnn_runtime_probe.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#ifdef __ANDROID__
#include <dlfcn.h>
#endif

namespace LSFG::Accelerator {
namespace {

#ifdef __ANDROID__

// Minimal ABI-only mirrors of the stable public provider prefixes. Keeping this
// local avoids a compile/link dependency on a Qualcomm SDK while still letting
// Phase 2 verify which provider the device runtime actually exposes.
using QnnErrorHandle = uint64_t;
constexpr QnnErrorHandle QNN_SUCCESS = 0;

struct QnnAbiVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
};

struct QnnAbiApiVersion {
    QnnAbiVersion coreApiVersion;
    QnnAbiVersion backendApiVersion;
};

struct QnnInterfacePrefix {
    uint32_t backendId;
    const char* providerName;
    QnnAbiApiVersion apiVersion;
};

struct QnnSystemInterfacePrefix {
    uint32_t backendId;
    const char* providerName;
    QnnAbiVersion systemApiVersion;
};

using QnnInterfaceGetProvidersFn = QnnErrorHandle (*)(
    const QnnInterfacePrefix*** providerList, uint32_t* numProviders);
using QnnSystemInterfaceGetProvidersFn = QnnErrorHandle (*)(
    const QnnSystemInterfacePrefix*** providerList, uint32_t* numProviders);

void* openOptionalRuntime(const char* library) {
    return dlopen(library, RTLD_NOW | RTLD_LOCAL);
}

std::string modulePathForSymbol(const void* symbol) {
    if (symbol == nullptr) return {};
    Dl_info info{};
    if (dladdr(symbol, &info) != 0 && info.dli_fname != nullptr)
        return info.dli_fname;
    return {};
}

bool containsHtp(const char* raw) {
    if (raw == nullptr || *raw == '\0') return false;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value.find("htp") != std::string::npos;
}

QnnVersion versionOf(const QnnAbiVersion& version) {
    return QnnVersion{version.major, version.minor, version.patch};
}

void qualifyHtpProvider(void* htpHandle, QnnRuntimeProbeResult& result) {
    dlerror();
    auto getProviders = reinterpret_cast<QnnInterfaceGetProvidersFn>(
        dlsym(htpHandle, "QnnInterface_getProviders"));
    result.qnnInterfaceSymbolFound = getProviders != nullptr;
    if (getProviders == nullptr) {
        result.failureReason = "qnn-interface-symbol-missing";
        return;
    }
    result.htpLibraryPath = modulePathForSymbol(reinterpret_cast<const void*>(getProviders));

    const QnnInterfacePrefix** providers = nullptr;
    uint32_t providerCount = 0;
    const QnnErrorHandle error = getProviders(&providers, &providerCount);
    result.qnnProviderCount = providerCount;
    result.qnnProviderEnumerated = error == QNN_SUCCESS && providers != nullptr && providerCount > 0;
    if (!result.qnnProviderEnumerated) {
        result.failureReason = "qnn-provider-enumeration-failed";
        return;
    }

    const QnnInterfacePrefix* selected = nullptr;
    for (uint32_t i = 0; i < providerCount; ++i) {
        const QnnInterfacePrefix* provider = providers[i];
        if (provider != nullptr && containsHtp(provider->providerName)) {
            selected = provider;
            break;
        }
    }
    if (selected == nullptr) {
        result.failureReason = "qnn-htp-provider-identity-unverified";
        return;
    }

    result.backendId = selected->backendId;
    result.providerName = selected->providerName == nullptr ? "" : selected->providerName;
    result.coreApiVersion = versionOf(selected->apiVersion.coreApiVersion);
    result.backendApiVersion = versionOf(selected->apiVersion.backendApiVersion);
    result.qnnProviderQualified = result.coreApiVersion.valid() && result.backendApiVersion.valid();
    if (!result.qnnProviderQualified)
        result.failureReason = "qnn-provider-version-invalid";
}

void qualifySystemProvider(void* systemHandle, QnnRuntimeProbeResult& result) {
    dlerror();
    auto getProviders = reinterpret_cast<QnnSystemInterfaceGetProvidersFn>(
        dlsym(systemHandle, "QnnSystemInterface_getProviders"));
    result.qnnSystemInterfaceSymbolFound = getProviders != nullptr;
    if (getProviders == nullptr) {
        result.failureReason = "qnn-system-interface-symbol-missing";
        return;
    }
    result.systemLibraryPath = modulePathForSymbol(reinterpret_cast<const void*>(getProviders));

    const QnnSystemInterfacePrefix** providers = nullptr;
    uint32_t providerCount = 0;
    const QnnErrorHandle error = getProviders(&providers, &providerCount);
    result.qnnSystemProviderCount = providerCount;
    result.qnnSystemProviderEnumerated =
        error == QNN_SUCCESS && providers != nullptr && providerCount > 0;
    if (!result.qnnSystemProviderEnumerated) {
        result.failureReason = "qnn-system-provider-enumeration-failed";
        return;
    }

    for (uint32_t i = 0; i < providerCount; ++i) {
        const QnnSystemInterfacePrefix* provider = providers[i];
        if (provider == nullptr) continue;
        const QnnVersion version = versionOf(provider->systemApiVersion);
        if (!version.valid()) continue;
        result.systemBackendId = provider->backendId;
        result.systemProviderName = provider->providerName == nullptr ? "" : provider->providerName;
        result.systemApiVersion = version;
        result.qnnSystemProviderQualified = true;
        return;
    }

    result.failureReason = "qnn-system-provider-version-invalid";
}

#endif

} // namespace

QnnRuntimeProbeResult probeQnnRuntimeMetadata(
        void*& qnnSystemHandle, void*& qnnHtpHandle) noexcept {
    QnnRuntimeProbeResult result{};

#ifdef __ANDROID__
    try {
        qnnSystemHandle = openOptionalRuntime("libQnnSystem.so");
        result.systemLibraryLoaded = qnnSystemHandle != nullptr;
        if (!result.systemLibraryLoaded) {
            result.failureReason = "qnn-system-library-unavailable";
            return result;
        }

        qnnHtpHandle = openOptionalRuntime("libQnnHtp.so");
        result.htpLibraryLoaded = qnnHtpHandle != nullptr;
        if (!result.htpLibraryLoaded) {
            result.failureReason = "qnn-htp-library-unavailable";
            return result;
        }

        qualifyHtpProvider(qnnHtpHandle, result);
        if (!result.qnnProviderQualified) return result;

        qualifySystemProvider(qnnSystemHandle, result);
        if (!result.qnnSystemProviderQualified) return result;

        result.failureReason.clear();
    } catch (...) {
        result.failureReason = "qnn-provider-probe-exception";
    }
#else
    (void)qnnSystemHandle;
    (void)qnnHtpHandle;
    result.failureReason = "qnn-android-runtime-unavailable";
#endif

    return result;
}

} // namespace LSFG::Accelerator
