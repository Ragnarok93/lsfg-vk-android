#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
context = (root / "src/context.cpp").read_text()
marker = "#else\n    // Desktop Linux path: OPAQUE_FD semaphore-based synchronization"
if marker not in context:
    raise SystemExit("desktop presentation path marker missing")
desktop = context.split(marker, 1)[1]
if "runtimeWaitTimeoutNs()" in desktop:
    raise SystemExit("desktop path must not depend on the Android-only runtime wait helper")
if "ovkAcquireNextImageKHR(info.device, this->swapchain, UINT64_MAX" not in desktop:
    raise SystemExit("desktop swapchain acquire must retain the upstream indefinite timeout")

cmake = (root / "CMakeLists.txt").read_text()
for android_only in (
    "src/android_diagnostics.cpp",
    "src/android_wsi_loader_bridge.cpp",
    "src/layer_android.cpp",
):
    if android_only not in cmake:
        raise SystemExit(f"desktop source exclusion missing for {android_only}")
if "else()\n    # Android loader/provenance entrypoints" not in cmake:
    raise SystemExit("Android-only source exclusions are not scoped to the desktop branch")

workflow = (root / ".github/workflows/android-bionic.yml").read_text()
for stale in ("b12-evidence-build:", "b12-mipmaps-refinement-build:"):
    if stale in workflow:
        raise SystemExit(f"stale profiling-only Android job remains: {stale}")
for token in (
    'git config user.name "github-actions[bot]"',
    'git config user.email "41898282+github-actions[bot]@users.noreply.github.com"',
):
    if token not in workflow:
        raise SystemExit(f"Android release tagging is missing identity configuration: {token}")

if (root / ".github/workflows/flatpak.yml").exists():
    raise SystemExit("stale Flatpak workflow still exists")
if (root / "scripts/flatpak").exists() and any((root / "scripts/flatpak").iterdir()):
    raise SystemExit("stale Flatpak source manifests still exist")

print("desktop/release CI portability contract: ok")
