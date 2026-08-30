from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = (ROOT / "VkLayer_LS_frame_generation.json").read_text()
BRIDGE = (ROOT / "src" / "android_wsi_loader_bridge.cpp").read_text()
DIAG = (ROOT / "src" / "android_diagnostics.cpp").read_text()


def test_android_manifest_routes_through_provenance_entrypoints():
    assert '"vkGetInstanceProcAddr": "lsfg_vkGetInstanceProcAddrDiagnostic"' in MANIFEST
    assert '"vkGetDeviceProcAddr": "lsfg_vkGetDeviceProcAddrDiagnostic"' in MANIFEST
    assert '"api_version": "1.3.0"' in MANIFEST


def test_wsi_provenance_captures_loader_pointer_identity_and_invocation():
    required = [
        "LSFG_PROVENANCE acquire",
        "LSFG_PROVENANCE invoke",
        "LSFG_PROVENANCE swapchain-create",
        "LSFG_PROVENANCE present",
        "LSFG_PROVENANCE destroy",
        "dispatchKey",
        "returned=",
        "target=",
        "module=",
        "exe=",
        "tid=",
        "vkCreateSwapchainKHR",
        "vkQueuePresentKHR",
        "vkDestroySwapchainKHR",
    ]
    for marker in required:
        assert marker in BRIDGE, marker


def test_diagnostics_log_process_and_loaded_layer_identity():
    for marker in ["LSFG_DIAG process", "cmdline=", "pid=", "tid=", "module="]:
        assert marker in DIAG, marker
