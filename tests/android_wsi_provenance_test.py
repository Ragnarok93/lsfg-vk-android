from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = (ROOT / "VkLayer_LS_frame_generation.json").read_text()
BRIDGE = (ROOT / "src" / "android_wsi_loader_bridge.cpp").read_text()
DIAG = (ROOT / "src" / "android_diagnostics.cpp").read_text()
CMAKE = (ROOT / "CMakeLists.txt").read_text()


def test_android_manifest_keeps_proven_xclipse_loader_contract():
    assert '"vkGetInstanceProcAddr": "layer_vkGetInstanceProcAddr"' in MANIFEST
    assert '"vkGetDeviceProcAddr": "layer_vkGetDeviceProcAddr"' in MANIFEST
    assert '"api_version": "1.3.0"' in MANIFEST
    assert '"lsfg_vkGetInstanceProcAddrDiagnostic"' not in MANIFEST
    assert '"lsfg_vkGetDeviceProcAddrDiagnostic"' not in MANIFEST


def test_production_entrypoints_are_transparently_instrumented():
    assert "layer_vkGetInstanceProcAddr=lsfg_layer_vkGetInstanceProcAddr_impl" in CMAKE
    assert "layer_vkGetDeviceProcAddr=lsfg_layer_vkGetDeviceProcAddr_impl" in CMAKE
    assert "PFN_vkVoidFunction layer_vkGetInstanceProcAddr" in BRIDGE
    assert "PFN_vkVoidFunction layer_vkGetDeviceProcAddr" in BRIDGE
    assert "lsfg_layer_vkGetInstanceProcAddr_impl" in BRIDGE
    assert "lsfg_layer_vkGetDeviceProcAddr_impl" in BRIDGE


def test_wsi_provenance_captures_pointer_identity_downstream_and_invocation():
    required = [
        "LSFG_PROVENANCE acquire",
        "LSFG_PROVENANCE invoke",
        "LSFG_PROVENANCE swapchain-create",
        "LSFG_PROVENANCE present",
        "LSFG_PROVENANCE destroy",
        "dispatchKey",
        "returned=",
        "returnedModule=",
        "target=",
        "targetModule=",
        "downstream=",
        "downstreamModule=",
        "module=",
        "exe=",
        "pid=",
        "tid=",
        "vkCreateSwapchainKHR",
        "vkQueuePresentKHR",
        "vkDestroySwapchainKHR",
        "sequence <= 12",
        "sequence % 120",
    ]
    for marker in required:
        assert marker in BRIDGE, marker


def test_diagnostics_log_process_and_loaded_layer_identity():
    for marker in ["LSFG_DIAG process", "LSFG_DIAG unload", "cmdline=", "pid=", "tid=", "module="]:
        assert marker in DIAG, marker
