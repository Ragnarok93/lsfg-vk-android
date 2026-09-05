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

const char* computeLibrary(QnnComputeBackendKind backend) {
    switch (backend) {
        case QnnComputeBackendKind::Htp: return "libQnnHtp.so";
        case QnnComputeBackendKind::Dsp: return "libQnnDsp.so";
        case QnnComputeBackendKind::None: break;
    }
    return nullptr;
}

const char* providerToken(QnnComputeBackendKind backend) {
    switch (backend) {
        case QnnComputeBackendKind::Htp: return "htp";
        case QnnComputeBackendKind::Dsp: return "dsp";
        case QnnComputeBackendKind::None: break;
    }
    return "";
}

void* openOptionalRuntime(const char* library) {
    return library == nullptr ? nullptr : dlopen(library, RTLD_NOW | RTLD_LOCAL);
}

std::string modulePathForSymbol(const void* symbol) {
    if (symbol == nullptr) return {};
    Dl_info info{};
    if (dladdr(symbol, &info) != 0 && info.dli_fname != nullptr)
        return info.dli_fname;
    return {};
}

bool containsInsensitive(const char* raw, const char* token) {
    if (raw == nullptr || *raw == '\0' || token == nullptr || *token == '\0') return false;
    std::string value(raw);
    std::string needle(token);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value.find(needle) != std::string::npos;
}

bool providerMatches(const QnnInterfacePrefix* provider, QnnComputeBackendKind backend) {
    if (provider == nullptr) return false;
    // providerName is explicitly allowed to be null by QNN. The symbol-origin
    // check in the smoke probe remains authoritative in that case.
    return provider->providerName == nullptr
        || containsInsensitive(provider->providerName, providerToken(backend));
}

QnnVersion versionOf(const QnnAbiVersion& version) {
    return QnnVersion{version.major, version.minor, version.patch};
}

void qualifyComputeProvider(void* computeHandle, QnnComputeBackendKind backend,
        QnnRuntimeProbeResult& result) {
    dlerror();
    auto getProviders = reinterpret_cast<QnnInterfaceGetProvidersFn>(
        dlsym(computeHandle, "QnnInterface_getProviders"));
    result.qnnInterfaceSymbolFound = getProviders != nullptr;
    if (getProviders == nullptr) {
        result.failureReason = "qnn-interface-symbol-missing";
        return;
    }
    result.computeLibraryPath = modulePathForSymbol(reinterpret_cast<const void*>(getProviders));

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
        if (providerMatches(providers[i], backend)) {
            selected = providers[i];
            break;
        }
    }
    if (selected == nullptr) {
        result.failureReason = std::string("qnn-") + qnnComputeBackendName(backend)
            + "-provider-identity-unverified";
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
        QnnComputeBackendKind computeBackend,
        void*& qnnSystemHandle,
        void*& qnnComputeHandle) noexcept {
    QnnRuntimeProbeResult result{};
    result.computeBackend = computeBackend;

#ifdef __ANDROID__
    try {
        qnnSystemHandle = openOptionalRuntime("libQnnSystem.so");
        result.systemLibraryLoaded = qnnSystemHandle != nullptr;
        if (!result.systemLibraryLoaded) {
            result.failureReason = "qnn-system-library-unavailable";
            return result;
        }

        const char* library = computeLibrary(computeBackend);
        qnnComputeHandle = openOptionalRuntime(library);
        result.computeLibraryLoaded = qnnComputeHandle != nullptr;
        if (!result.computeLibraryLoaded) {
            result.failureReason = std::string("qnn-") + qnnComputeBackendName(computeBackend)
                + "-library-unavailable";
            return result;
        }

        qualifyComputeProvider(qnnComputeHandle, computeBackend, result);
        if (!result.qnnProviderQualified) return result;

        qualifySystemProvider(qnnSystemHandle, result);
        if (!result.qnnSystemProviderQualified) return result;

        result.failureReason.clear();
    } catch (...) {
        result.failureReason = "qnn-provider-probe-exception";
    }
#else
    (void)qnnSystemHandle;
    (void)qnnComputeHandle;
    result.failureReason = "qnn-android-runtime-unavailable";
#endif

    return result;
}

const char* qnnComputeBackendName(QnnComputeBackendKind backend) noexcept {
    switch (backend) {
        case QnnComputeBackendKind::None: return "none";
        case QnnComputeBackendKind::Htp: return "htp";
        case QnnComputeBackendKind::Dsp: return "dsp";
    }
    return "none";
}

} // namespace LSFG::Accelerator
