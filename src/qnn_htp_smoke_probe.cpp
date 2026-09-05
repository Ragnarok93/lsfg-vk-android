#include "qnn_htp_smoke_probe.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#endif

namespace LSFG::Accelerator {
namespace {

#ifdef __ANDROID__

using QnnErrorHandle = uint64_t;
using QnnHandle = void*;
using GenericFn = void (*)();
constexpr QnnErrorHandle QNN_SUCCESS = 0;
constexpr uint32_t QNN_CORE_ABI_MAJOR = 2;
constexpr uint32_t QNN_TENSOR_VERSION_1 = 1;
constexpr uint32_t QNN_OPCONFIG_VERSION_1 = 1;
constexpr uint32_t QNN_TENSOR_TYPE_APP_WRITE = 0;
constexpr uint32_t QNN_TENSOR_TYPE_APP_READ = 1;
constexpr uint32_t QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER = 0;
constexpr uint32_t QNN_DATATYPE_SFIXED_POINT_8 = 0x0308;
constexpr uint32_t QNN_DEFINITION_DEFINED = 1;
constexpr uint32_t QNN_QUANTIZATION_ENCODING_SCALE_OFFSET = 0;
constexpr uint32_t QNN_TENSORMEMTYPE_RAW = 0;
constexpr uint32_t QNN_TENSORMEMTYPE_MEMHANDLE = 1;
constexpr uint32_t QNN_MEM_TYPE_ION = 1;

struct QnnAbiVersion { uint32_t major; uint32_t minor; uint32_t patch; };
struct QnnAbiApiVersion { QnnAbiVersion coreApiVersion; QnnAbiVersion backendApiVersion; };
struct QnnScaleOffset { float scale; int32_t offset; };
struct QnnAxisScaleOffset { int32_t axis; uint32_t numScaleOffsets; QnnScaleOffset* scaleOffset; };
struct QnnBwScaleOffset { uint32_t bitwidth; float scale; int32_t offset; };
struct QnnBwAxisScaleOffset {
    uint32_t bitwidth; int32_t axis; uint32_t numElements; float* scales; int32_t* offsets;
};
struct QnnQuantizeParams {
    uint32_t encodingDefinition;
    uint32_t quantizationEncoding;
    union {
        QnnScaleOffset scaleOffsetEncoding;
        QnnAxisScaleOffset axisScaleOffsetEncoding;
        QnnBwScaleOffset bwScaleOffsetEncoding;
        QnnBwAxisScaleOffset bwAxisScaleOffsetEncoding;
    };
};
struct QnnClientBuffer { void* data; uint32_t dataSize; };
struct QnnTensorV1 {
    uint32_t id;
    const char* name;
    uint32_t type;
    uint32_t dataFormat;
    uint32_t dataType;
    QnnQuantizeParams quantizeParams;
    uint32_t rank;
    uint32_t* dimensions;
    uint32_t memType;
    union { QnnClientBuffer clientBuf; QnnHandle memHandle; };
};
struct QnnTensor { uint32_t version; union { QnnTensorV1 v1; }; };
struct QnnOpConfigV1 {
    const char* name;
    const char* packageName;
    const char* typeName;
    uint32_t numOfParams;
    void* params;
    uint32_t numOfInputs;
    QnnTensor* inputTensors;
    uint32_t numOfOutputs;
    QnnTensor* outputTensors;
};
struct QnnOpConfig { uint32_t version; union { QnnOpConfigV1 v1; }; };
struct QnnMemShape { uint32_t numDim; uint32_t* dimSize; const char* shapeConfig; };
struct QnnMemIonInfo { int32_t fd; };
struct QnnMemDescriptor {
    QnnMemShape memShape;
    uint32_t dataType;
    uint32_t memType;
    union { QnnMemIonInfo ionInfo; void* customInfo; };
};

using BackendCreateFn = QnnErrorHandle (*)(QnnHandle, const void* const*, QnnHandle*);
using BackendGetBuildIdFn = QnnErrorHandle (*)(const char**);
using BackendFreeFn = QnnErrorHandle (*)(QnnHandle);
using ContextCreateFn = QnnErrorHandle (*)(QnnHandle, QnnHandle, const void* const*, QnnHandle*);
using ContextFreeFn = QnnErrorHandle (*)(QnnHandle, QnnHandle);
using GraphCreateFn = QnnErrorHandle (*)(QnnHandle, const char*, const void* const*, QnnHandle*);
using GraphAddNodeFn = QnnErrorHandle (*)(QnnHandle, QnnOpConfig);
using GraphFinalizeFn = QnnErrorHandle (*)(QnnHandle, QnnHandle, QnnHandle);
using GraphExecuteFn = QnnErrorHandle (*)(QnnHandle, const QnnTensor*, uint32_t,
    QnnTensor*, uint32_t, QnnHandle, QnnHandle);
using TensorCreateGraphTensorFn = QnnErrorHandle (*)(QnnHandle, QnnTensor*);
using MemRegisterFn = QnnErrorHandle (*)(QnnHandle, const QnnMemDescriptor*, uint32_t, QnnHandle*);
using MemDeRegisterFn = QnnErrorHandle (*)(const QnnHandle*, uint32_t);
using DeviceCreateFn = QnnErrorHandle (*)(QnnHandle, const void* const*, QnnHandle*);
using DeviceFreeFn = QnnErrorHandle (*)(QnnHandle);

// Stable QNN v2 interface prefix through deviceFree. Newer interface members
// are appended after this prefix and are intentionally not dereferenced here.
struct QnnInterfaceImplementation {
    GenericFn propertyHasCapability;
    BackendCreateFn backendCreate;
    GenericFn backendGetApiVersion;
    BackendGetBuildIdFn backendGetBuildId;
    GenericFn backendRegisterOpPackage;
    GenericFn backendGetSupportedOperations;
    GenericFn backendValidateOpConfig;
    BackendFreeFn backendFree;
    ContextCreateFn contextCreate;
    GenericFn contextSetConfig;
    GenericFn contextGetBinarySize;
    GenericFn contextGetBinary;
    GenericFn contextCreateFromBinary;
    ContextFreeFn contextFree;
    GraphCreateFn graphCreate;
    GenericFn graphSetConfig;
    GraphAddNodeFn graphAddNode;
    GraphFinalizeFn graphFinalize;
    GenericFn graphRetrieve;
    GraphExecuteFn graphExecute;
    GenericFn graphExecuteAsync;
    GenericFn tensorCreateContextTensor;
    TensorCreateGraphTensorFn tensorCreateGraphTensor;
    GenericFn logCreate;
    GenericFn logSetLogLevel;
    GenericFn logFree;
    GenericFn profileCreate;
    GenericFn profileGetEvents;
    GenericFn profileGetSubEvents;
    GenericFn profileGetEventData;
    GenericFn profileFree;
    MemRegisterFn memRegister;
    MemDeRegisterFn memDeRegister;
    GenericFn deviceGetPlatformInfo;
    GenericFn deviceFreePlatformInfo;
    GenericFn deviceGetInfrastructure;
    DeviceCreateFn deviceCreate;
    GenericFn deviceSetConfig;
    GenericFn deviceGetInfo;
    DeviceFreeFn deviceFree;
};
struct QnnInterfaceProvider {
    uint32_t backendId;
    const char* providerName;
    QnnAbiApiVersion apiVersion;
    union { QnnInterfaceImplementation implementation; };
};
using GetProvidersFn = QnnErrorHandle (*)(const QnnInterfaceProvider***, uint32_t*);

struct NativeHandlePrefix { int version; int numFds; int numInts; int data[1]; };
using AHardwareBufferGetNativeHandleFn = const NativeHandlePrefix* (*)(const AHardwareBuffer*);

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

const char* expectedModuleName(QnnComputeBackendKind backend) {
    switch (backend) {
        case QnnComputeBackendKind::Htp: return "libqnnhtp.so";
        case QnnComputeBackendKind::Dsp: return "libqnndsp.so";
        case QnnComputeBackendKind::None: break;
    }
    return "";
}

bool providerMatches(const QnnInterfaceProvider* provider, QnnComputeBackendKind backend) {
    if (provider == nullptr) return false;
    if (provider->providerName == nullptr) return true;
    const std::string name = lower(provider->providerName);
    return name.find(qnnComputeBackendName(backend)) != std::string::npos;
}

bool symbolComesFromComputeBackend(const void* symbol, QnnComputeBackendKind backend) {
    if (symbol == nullptr) return false;
    Dl_info info{};
    if (dladdr(symbol, &info) == 0 || info.dli_fname == nullptr) return false;
    const std::string path = lower(info.dli_fname);
    return path.find(expectedModuleName(backend)) != std::string::npos;
}

QnnQuantizeParams signedInt8Quantization() {
    QnnQuantizeParams quant{};
    quant.encodingDefinition = QNN_DEFINITION_DEFINED;
    quant.quantizationEncoding = QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;
    quant.scaleOffsetEncoding = QnnScaleOffset{1.0f, 0};
    return quant;
}

QnnTensor graphTensor(const char* name, uint32_t type, uint32_t* dimensions) {
    QnnTensor tensor{};
    tensor.version = QNN_TENSOR_VERSION_1;
    tensor.v1.id = 0;
    tensor.v1.name = name;
    tensor.v1.type = type;
    tensor.v1.dataFormat = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER;
    tensor.v1.dataType = QNN_DATATYPE_SFIXED_POINT_8;
    tensor.v1.quantizeParams = signedInt8Quantization();
    tensor.v1.rank = 1;
    tensor.v1.dimensions = dimensions;
    tensor.v1.memType = QNN_TENSORMEMTYPE_RAW;
    tensor.v1.clientBuf = QnnClientBuffer{nullptr, 0};
    return tensor;
}

struct AhbBuffer { AHardwareBuffer* buffer{nullptr}; int fd{-1}; };

void releaseAhb(AhbBuffer& buffer) noexcept {
    if (buffer.buffer != nullptr) {
        AHardwareBuffer_release(buffer.buffer);
        buffer.buffer = nullptr;
    }
    buffer.fd = -1;
}

bool allocateShareableAhb(AHardwareBufferGetNativeHandleFn getNativeHandle,
        AhbBuffer& out, std::string& failure) {
    AHardwareBuffer_Desc desc{};
    desc.width = 4096;
    desc.height = 1;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
        | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
        | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    if (AHardwareBuffer_allocate(&desc, &out.buffer) != 0 || out.buffer == nullptr) {
        failure = "ahb-shared-buffer-allocation-failed";
        return false;
    }
    const NativeHandlePrefix* nativeHandle = getNativeHandle(out.buffer);
    if (nativeHandle == nullptr || nativeHandle->numFds < 1 || nativeHandle->data[0] < 0) {
        failure = "ahb-native-dmabuf-unavailable";
        releaseAhb(out);
        return false;
    }
    out.fd = nativeHandle->data[0];
    return true;
}

bool writeInputAhb(AhbBuffer& input, const int8_t (&values)[4], std::string& failure) {
    void* address = nullptr;
    if (AHardwareBuffer_lock(input.buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
            -1, nullptr, &address) != 0 || address == nullptr) {
        failure = "ahb-input-lock-failed";
        return false;
    }
    std::memcpy(address, values, sizeof(values));
    if (AHardwareBuffer_unlock(input.buffer, nullptr) != 0) {
        failure = "ahb-input-unlock-failed";
        return false;
    }
    return true;
}

bool verifyOutputAhb(AhbBuffer& output, std::string& failure) {
    void* address = nullptr;
    if (AHardwareBuffer_lock(output.buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            -1, nullptr, &address) != 0 || address == nullptr) {
        failure = "ahb-output-lock-failed";
        return false;
    }
    const int8_t expected[4] = {0, 0, 3, 12};
    const bool matches = std::memcmp(address, expected, sizeof(expected)) == 0;
    if (AHardwareBuffer_unlock(output.buffer, nullptr) != 0) {
        failure = "ahb-output-unlock-failed";
        return false;
    }
    if (!matches) {
        failure = "numerical-smoke-mismatch";
        return false;
    }
    return true;
}

QnnMemDescriptor ionDescriptor(int fd, uint32_t* dimensions) {
    QnnMemDescriptor descriptor{};
    descriptor.memShape = QnnMemShape{1, dimensions, nullptr};
    descriptor.dataType = QNN_DATATYPE_SFIXED_POINT_8;
    descriptor.memType = QNN_MEM_TYPE_ION;
    descriptor.ionInfo = QnnMemIonInfo{fd};
    return descriptor;
}

#endif

} // namespace

QnnComputeSmokeResult probeQnnComputeGraphAndSharedMemory(
        void* qnnComputeHandle, QnnComputeBackendKind computeBackend) noexcept {
    QnnComputeSmokeResult result{};
#ifdef __ANDROID__
    if (qnnComputeHandle == nullptr || computeBackend == QnnComputeBackendKind::None) {
        result.failureReason = "qnn-compute-library-unavailable";
        return result;
    }

    QnnHandle backend = nullptr;
    QnnHandle device = nullptr;
    QnnHandle context = nullptr;
    QnnHandle inputMem = nullptr;
    QnnHandle outputMem = nullptr;
    AhbBuffer inputAhb{};
    AhbBuffer outputAhb{};
    const QnnInterfaceImplementation* api = nullptr;

    const auto cleanup = [&]() noexcept {
        if (api != nullptr && api->memDeRegister != nullptr) {
            if (outputMem != nullptr) { QnnHandle h = outputMem; api->memDeRegister(&h, 1); outputMem = nullptr; }
            if (inputMem != nullptr) { QnnHandle h = inputMem; api->memDeRegister(&h, 1); inputMem = nullptr; }
        }
        if (api != nullptr && api->contextFree != nullptr && context != nullptr) {
            api->contextFree(context, nullptr); context = nullptr;
        }
        if (api != nullptr && api->deviceFree != nullptr && device != nullptr) {
            api->deviceFree(device); device = nullptr;
        }
        if (api != nullptr && api->backendFree != nullptr && backend != nullptr) {
            api->backendFree(backend); backend = nullptr;
        }
        releaseAhb(outputAhb);
        releaseAhb(inputAhb);
    };

    try {
        auto getProviders = reinterpret_cast<GetProvidersFn>(
            dlsym(qnnComputeHandle, "QnnInterface_getProviders"));
        if (getProviders == nullptr || !symbolComesFromComputeBackend(
                reinterpret_cast<const void*>(getProviders), computeBackend)) {
            result.failureReason = std::string("qnn-") + qnnComputeBackendName(computeBackend)
                + "-provider-origin-unverified";
            cleanup();
            return result;
        }

        const QnnInterfaceProvider** providers = nullptr;
        uint32_t providerCount = 0;
        if (getProviders(&providers, &providerCount) != QNN_SUCCESS
                || providers == nullptr || providerCount == 0) {
            result.failureReason = "qnn-compute-provider-enumeration-failed";
            cleanup();
            return result;
        }
        const QnnInterfaceProvider* selected = nullptr;
        for (uint32_t i = 0; i < providerCount; ++i) {
            if (providerMatches(providers[i], computeBackend)) { selected = providers[i]; break; }
        }
        if (selected == nullptr) {
            result.failureReason = "qnn-compute-provider-identity-unverified";
            cleanup();
            return result;
        }
        if (selected->apiVersion.coreApiVersion.major != QNN_CORE_ABI_MAJOR) {
            result.failureReason = "qnn-core-abi-major-unsupported";
            cleanup();
            return result;
        }
        result.computeAttributionQualified = true;
        api = &selected->implementation;
        if (api->backendCreate == nullptr || api->backendFree == nullptr
                || api->contextCreate == nullptr || api->contextFree == nullptr
                || api->graphCreate == nullptr || api->graphAddNode == nullptr
                || api->graphFinalize == nullptr || api->graphExecute == nullptr
                || api->tensorCreateGraphTensor == nullptr
                || api->memRegister == nullptr || api->memDeRegister == nullptr) {
            result.failureReason = "qnn-phase2-required-interface-missing";
            cleanup();
            return result;
        }
        if (api->backendGetBuildId != nullptr) {
            const char* buildId = nullptr;
            if (api->backendGetBuildId(&buildId) == QNN_SUCCESS && buildId != nullptr)
                result.backendBuildId = buildId;
        }
        if (api->backendCreate(nullptr, nullptr, &backend) != QNN_SUCCESS || backend == nullptr) {
            result.failureReason = "qnn-compute-backend-create-failed";
            cleanup();
            return result;
        }
        if (api->deviceCreate != nullptr) {
            QnnHandle candidate = nullptr;
            if (api->deviceCreate(nullptr, nullptr, &candidate) == QNN_SUCCESS) device = candidate;
        }
        if (api->contextCreate(backend, device, nullptr, &context) != QNN_SUCCESS || context == nullptr) {
            result.failureReason = "qnn-compute-context-create-failed";
            cleanup();
            return result;
        }

        auto getNativeHandle = reinterpret_cast<AHardwareBufferGetNativeHandleFn>(
            dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle"));
        if (getNativeHandle == nullptr) {
            result.failureReason = "ahb-native-handle-api-unavailable";
            cleanup();
            return result;
        }
        if (!allocateShareableAhb(getNativeHandle, inputAhb, result.failureReason)
                || !allocateShareableAhb(getNativeHandle, outputAhb, result.failureReason)) {
            cleanup();
            return result;
        }
        const int8_t inputValues[4] = {-4, 0, 3, 12};
        if (!writeInputAhb(inputAhb, inputValues, result.failureReason)) {
            cleanup();
            return result;
        }

        uint32_t dimensions[1] = {4};
        const QnnMemDescriptor inputDescriptor = ionDescriptor(inputAhb.fd, dimensions);
        const QnnMemDescriptor outputDescriptor = ionDescriptor(outputAhb.fd, dimensions);
        if (api->memRegister(context, &inputDescriptor, 1, &inputMem) != QNN_SUCCESS || inputMem == nullptr) {
            result.failureReason = "qnn-ahb-input-mem-register-failed";
            cleanup();
            return result;
        }
        if (api->memRegister(context, &outputDescriptor, 1, &outputMem) != QNN_SUCCESS || outputMem == nullptr) {
            result.failureReason = "qnn-ahb-output-mem-register-failed";
            cleanup();
            return result;
        }
        result.sharedMemoryQualified = true;

        QnnHandle graph = nullptr;
        if (api->graphCreate(context, "lsfg_phase2_compute_smoke", nullptr, &graph) != QNN_SUCCESS || graph == nullptr) {
            result.failureReason = "qnn-smoke-graph-create-failed";
            cleanup();
            return result;
        }
        QnnTensor inputTensor = graphTensor("phase2_input", QNN_TENSOR_TYPE_APP_WRITE, dimensions);
        QnnTensor outputTensor = graphTensor("phase2_output", QNN_TENSOR_TYPE_APP_READ, dimensions);
        if (api->tensorCreateGraphTensor(graph, &inputTensor) != QNN_SUCCESS
                || api->tensorCreateGraphTensor(graph, &outputTensor) != QNN_SUCCESS) {
            result.failureReason = "qnn-smoke-tensor-create-failed";
            cleanup();
            return result;
        }
        QnnOpConfig op{};
        op.version = QNN_OPCONFIG_VERSION_1;
        op.v1.name = "lsfg_phase2_relu";
        op.v1.packageName = "qti.aisw";
        op.v1.typeName = "Relu";
        op.v1.numOfInputs = 1;
        op.v1.inputTensors = &inputTensor;
        op.v1.numOfOutputs = 1;
        op.v1.outputTensors = &outputTensor;
        if (api->graphAddNode(graph, op) != QNN_SUCCESS) {
            result.failureReason = "qnn-smoke-graph-add-node-failed";
            cleanup();
            return result;
        }
        if (api->graphFinalize(graph, nullptr, nullptr) != QNN_SUCCESS) {
            result.failureReason = "qnn-smoke-graph-finalize-failed";
            cleanup();
            return result;
        }
        QnnTensor executeInput = inputTensor;
        executeInput.v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE;
        executeInput.v1.memHandle = inputMem;
        QnnTensor executeOutput = outputTensor;
        executeOutput.v1.memType = QNN_TENSORMEMTYPE_MEMHANDLE;
        executeOutput.v1.memHandle = outputMem;
        if (api->graphExecute(graph, &executeInput, 1, &executeOutput, 1, nullptr, nullptr) != QNN_SUCCESS) {
            result.failureReason = "qnn-smoke-graph-execute-failed";
            cleanup();
            return result;
        }
        result.graphExecutionQualified = true;
        if (!verifyOutputAhb(outputAhb, result.failureReason)) {
            cleanup();
            return result;
        }
        result.numericalSmokeQualified = true;
        result.failureReason.clear();
        cleanup();
    } catch (...) {
        result.failureReason = "qnn-phase2-smoke-exception";
        cleanup();
    }
#else
    (void)qnnComputeHandle;
    (void)computeBackend;
    result.failureReason = "qnn-android-runtime-unavailable";
#endif
    return result;
}

} // namespace LSFG::Accelerator
