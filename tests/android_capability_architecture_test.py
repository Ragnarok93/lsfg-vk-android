#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCapabilityArchitectureContractTest(unittest.TestCase):
    def test_real_device_and_driver_uuid_replace_vendor_device_surrogate(self) -> None:
        utils = (ROOT / "src/utils/utils.cpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        combined = utils + device
        self.assertIn("VkPhysicalDeviceIDProperties", combined)
        self.assertIn("deviceUUID", combined)
        self.assertIn("driverUUID", combined)
        self.assertNotIn("static_cast<uint64_t>(properties.vendorID) << 32", utils)
        self.assertNotIn("deviceUUID == 0x1463ABAC", device)

    def test_optional_device_extensions_are_probed_from_advertised_extensions(self) -> None:
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "hasExtension(availableExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)",
            device,
        )
        self.assertNotIn(
            "hasExtension(enabledExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)",
            device,
        )

    def test_ahb_storage_contract_is_probed_and_allocated_correctly(self) -> None:
        image = (ROOT / "src/mini/image.cpp").read_text(encoding="utf-8")
        device = (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        self.assertIn("AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER", image)
        self.assertIn("vkGetPhysicalDeviceImageFormatProperties2", device)
        self.assertIn("VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID", device)
        self.assertIn("VK_IMAGE_USAGE_STORAGE_BIT", device)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", device)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", device)
        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(encoding="utf-8")
        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")
        for source in (quality, performance):
            self.assertIn("transportOnly", source)
            self.assertIn("sharedOutImages", source)
            self.assertIn("vkCmdCopyImage", source)
            self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", source)
            self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", source)

    def test_swapchain_capacity_is_multiplier_driven_and_fails_open(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("requiredHeadroom", hooks)
        self.assertIn("activeConf.multiplier", hooks)
        self.assertIn("swapchain-insufficient-headroom", hooks)
        self.assertNotIn("createInfo.minImageCount = pCreateInfo->minImageCount + 1;", hooks)

    def test_android_runtime_waits_are_bounded(self) -> None:
        wrapper = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(encoding="utf-8")
        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")
        quality_lifecycle = (ROOT / "framegen/v3.1_src/lsfg.cpp").read_text(encoding="utf-8")
        performance_lifecycle = (ROOT / "framegen/v3.1p_src/lsfg.cpp").read_text(encoding="utf-8")
        self.assertNotIn("UINT64_MAX", wrapper)
        self.assertNotIn("UINT64_MAX", quality)
        self.assertNotIn("UINT64_MAX", performance)
        self.assertNotIn("vkDeviceWaitIdle", quality_lifecycle)
        self.assertNotIn("vkDeviceWaitIdle", performance_lifecycle)

    def test_blit_capabilities_are_queried_before_use(self) -> None:
        combined = (
            (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
            + (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("vkGetPhysicalDeviceFormatProperties", combined)
        self.assertIn("VK_FORMAT_FEATURE_BLIT_SRC_BIT", combined)
        self.assertIn("VK_FORMAT_FEATURE_BLIT_DST_BIT", combined)

    def test_phase2_accelerator_is_opt_in_qualified_and_vulkan_fail_open(self) -> None:
        coordinator = (ROOT / "src/accelerator_coordinator.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/accelerator_coordinator.hpp").read_text(encoding="utf-8")
        qnn_probe = (ROOT / "src/qnn_runtime_probe.cpp").read_text(encoding="utf-8")
        qnn_probe_header = (ROOT / "include/qnn_runtime_probe.hpp").read_text(encoding="utf-8")
        interface = (ROOT / "include/frame_generation_backend.hpp").read_text(encoding="utf-8")
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("FrameGenerationComputeBackend", interface)
        self.assertIn("LSFG_NPU_ACCELERATION", coordinator)
        self.assertIn("libQnnSystem.so", qnn_probe)
        self.assertIn("libQnnHtp.so", qnn_probe)
        # SM8250 / Snapdragon 865 is Hexagon v66 and uses the QNN DSP backend,
        # while SM8350+ devices use QNN HTP. Both must remain valid candidates.
        self.assertIn("libQnnDsp.so", qnn_probe)
        # Phase 2 must be able to consume an app-local QAIRT deployment without
        # changing Android's global linker state, and must preserve the exact
        # loader error for diagnostics when neither namespace nor app-local load works.
        self.assertIn("LSFG_QNN_RUNTIME_DIR", qnn_probe)
        self.assertIn("std::getenv", qnn_probe)
        self.assertIn("dlerror", qnn_probe)
        self.assertIn("systemLoadDiagnostic", qnn_probe_header)
        self.assertIn("computeLoadDiagnostic", qnn_probe_header)
        self.assertIn("load_detail=", coordinator)
        self.assertNotIn("LD_LIBRARY_PATH", qnn_probe)
        self.assertIn("QnnComputeBackendKind", qnn_probe_header)
        self.assertIn("Dsp", qnn_probe_header)
        self.assertIn("Htp", qnn_probe_header)
        self.assertIn("QnnComputeBackendKind::Htp", coordinator)
        self.assertIn("QnnComputeBackendKind::Dsp", coordinator)
        self.assertIn("QnnInterface_getProviders", qnn_probe)
        self.assertIn("QnnSystemInterface_getProviders", qnn_probe)
        self.assertIn("providerName", qnn_probe)
        self.assertIn("coreApiVersion", qnn_probe)
        self.assertIn("backendApiVersion", qnn_probe)
        self.assertIn("systemApiVersion", qnn_probe)
        self.assertIn("dladdr", qnn_probe)
        self.assertIn("qnnProviderQualified", header)
        self.assertIn("qnnSystemProviderQualified", header)
        self.assertIn("qnnComputeBackend", header)
        self.assertIn("directAhbInteropQualified", header)
        self.assertIn("phase2-execution-disabled", coordinator)
        self.assertIn("accelerator_module_version=phase2", coordinator)
        self.assertIn("selectedBackend = BackendKind::Vulkan", coordinator)
        self.assertIn("executionEnabled = false", coordinator)
        self.assertIn("if (!status_.npuSettingEnabled)", coordinator)
        self.assertIn("if (conf.multiplier > 1)", main)
        self.assertIn("AcceleratorCoordinator::instance().beginEligibleSession()", main)
        self.assertNotIn("QnnSystem", cmake)
        self.assertNotIn("QnnHtp", cmake)
        self.assertNotIn("QnnDsp", cmake)
        self.assertNotIn("SNPE", cmake)

    def test_phase2_qnn_smoke_proves_compute_graph_ahb_and_numerical_path(self) -> None:
        smoke = (ROOT / "src/qnn_htp_smoke_probe.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/accelerator_coordinator.hpp").read_text(encoding="utf-8")
        coordinator = (ROOT / "src/accelerator_coordinator.cpp").read_text(encoding="utf-8")

        for token in (
            "AHardwareBuffer_getNativeHandle",
            "AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER",
            "QNN_MEM_TYPE_ION",
            "memRegister",
            "graphCreate",
            "graphAddNode",
            "graphFinalize",
            "graphExecute",
            "qti.aisw",
            "Relu",
            "numerical-smoke-mismatch",
            "QnnComputeBackendKind",
            "libqnnhtp.so",
            "libqnndsp.so",
        ):
            self.assertIn(token, smoke)

        for token in (
            "qnnComputeAttributionQualified",
            "qnnSharedMemoryQualified",
            "qnnGraphExecutionQualified",
            "qnnNumericalSmokeQualified",
        ):
            self.assertIn(token, header)
            self.assertIn(token, coordinator)

        self.assertIn("phase3-dynamic-warp-benchmark-required", coordinator)
        self.assertIn("executionEnabled = false", coordinator)
        self.assertIn("selectedBackend = BackendKind::Vulkan", coordinator)

    def test_accelerator_events_use_existing_native_diagnostics_artifact(self) -> None:
        diagnostics = (ROOT / "src/android_diagnostics.cpp").read_text(encoding="utf-8")
        coordinator = (ROOT / "src/accelerator_coordinator.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/android_diagnostics.hpp").read_text(encoding="utf-8")

        self.assertIn("logNativeDiagnostic", header)
        self.assertIn("LSFG_CONFIG", diagnostics)
        self.assertIn("diagnostics.log", diagnostics)
        self.assertIn("O_APPEND", diagnostics)
        self.assertIn("logNativeDiagnostic(message)", coordinator)

    def test_initialization_telemetry_contains_required_provenance_and_capacity(self) -> None:
        combined = (
            (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
            + (ROOT / "framegen/src/core/device.cpp").read_text(encoding="utf-8")
        )
        for token in (
            "driverUUID", "deviceUUID", "driverName", "driverVersion",
            "ahbR16fStorage", "requiredHeadroom", "minImageCount", "maxImageCount"
        ):
            self.assertIn(token, combined)


if __name__ == "__main__":
    unittest.main()
