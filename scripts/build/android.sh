#!/usr/bin/env bash
# Build lsfg-vk for GameNative's Android/Bionic Wine-on-Android runtime.
#
# Produces by default:
#   build-android-${ANDROID_ABI}/liblsfg-vk.so
#   build-android-${ANDROID_ABI}/dist/liblsfg-vk-${ANDROID_ABI}.so (stripped)
#   build-android-${ANDROID_ABI}/dist/VkLayer_LS_frame_generation.json
#
# Requirements:
#   ANDROID_NDK env var pointing at an Android NDK r25+.
#
# Usage:
#   ANDROID_NDK=/path/to/android-ndk-r27d ./scripts/build/android.sh [Release|Debug]
#   ANDROID_ABI=x86_64 ANDROID_NDK=/path/to/android-ndk-r27d ./scripts/build/android.sh
#   LSFGVK_ADAPTIVE_RUNTIME=1 ... ./scripts/build/android.sh       # clean retained adaptive runtime
#   LSFGVK_ZERO_STAGE_PROFILE=1 ... ./scripts/build/android.sh     # profiling build
#   LSFGVK_ZERO_STAGE_PROFILE=1 LSFGVK_B8_DIAGNOSTICS=1 ...       # legacy B8 diagnostic matrix
#   LSFGVK_ZERO_STAGE_PROFILE=1 LSFGVK_FINAL_NONADAPTIVE_SWEEP=1 ... # deferred final sweep
#   LSFGVK_EXPERIMENTAL_B9=1 ... ./scripts/build/android.sh        # experimental Beta4 scheduler

set -euo pipefail

BUILD_TYPE="${1:-Release}"
ABI="${ANDROID_ABI:-arm64-v8a}"
API="${ANDROID_PLATFORM:-android-28}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

if [[ -z "${ANDROID_NDK:-}" ]]; then
    echo "error: ANDROID_NDK must be set to your NDK root (e.g. /opt/android-ndk-r27d)" >&2
    exit 1
fi

TOOLCHAIN="${ANDROID_NDK}/build/cmake/android.toolchain.cmake"
if [[ ! -f "${TOOLCHAIN}" ]]; then
    echo "error: toolchain not found at ${TOOLCHAIN}" >&2
    exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-android-${ABI}}"
DIST_DIR="${DIST_DIR:-${BUILD_DIR}/dist}"

python3 "${REPO_ROOT}/scripts/adreno_suspend_timeout_guard.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/adreno_android_runtime_residency.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/adreno_android_config_reload.py" --root "${REPO_ROOT}"

if [[ "${LSFGVK_ADAPTIVE_RUNTIME:-0}" == "1" ]]; then
    echo "[lsfg-vk] Enabling retained adaptive runtime without GPU/compiler profiling"
    python3 "${REPO_ROOT}/scripts/apply-adreno-evidence-profile.py" \
        --root "${REPO_ROOT}" --runtime-only
fi

if [[ "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]; then
    if [[ "${LSFGVK_ADAPTIVE_RUNTIME:-0}" == "1" ]]; then
        echo "[lsfg-vk] Ignoring profiling request because clean adaptive runtime mode is active"
    else
        echo "[lsfg-vk] Enabling temporary zero-stage GPU profiling instrumentation"
        python3 "${REPO_ROOT}/scripts/apply-zero-stage-profile.py" --root "${REPO_ROOT}"
        python3 "${REPO_ROOT}/scripts/apply-mipmaps-shader-profile.py" --root "${REPO_ROOT}"
        python3 "${REPO_ROOT}/scripts/apply-adreno-evidence-profile.py" --root "${REPO_ROOT}"
        python3 "${REPO_ROOT}/scripts/apply-candidate-b-shader-hot-path.py" --root "${REPO_ROOT}"
        python3 "${REPO_ROOT}/scripts/apply-candidate-b2-mipmaps-dependency-profile.py" --root "${REPO_ROOT}"
        python3 "${REPO_ROOT}/scripts/apply-candidate-b3-beta4-analysis.py" --root "${REPO_ROOT}"
        if [[ "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]; then
            echo "[lsfg-vk] Enabling deferred final non-adaptive compiler sweep"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-local-spirv-constants.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-final-nonadaptive-sweep-v2.py" --root "${REPO_ROOT}"
        elif [[ "${LSFGVK_B8_DIAGNOSTICS:-0}" == "1" ]]; then
            echo "[lsfg-vk] Enabling opt-in B8 pipeline-executable and mipmaps-matrix diagnostics"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-local-spirv-constants.py" --root "${REPO_ROOT}"
        fi
    fi
fi

python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/apply-candidate-b11-beta4-pow2-mask.py" --root "${REPO_ROOT}"
if [[ "${LSFGVK_EXPERIMENTAL_B9:-0}" == "1" ]]; then
    echo "[lsfg-vk] Enabling experimental B9 Beta4 scheduler"
    python3 "${REPO_ROOT}/scripts/apply-candidate-b9-beta4-spill-collapse.py" --root "${REPO_ROOT}"
fi

# Apply after all optional source transforms so command-buffer reuse cannot
# invalidate their source anchors. This is Android-only build composition.
python3 "${REPO_ROOT}/scripts/apply-android-command-buffer-reuse.py" --root "${REPO_ROOT}"

mkdir -p "${BUILD_DIR}" "${DIST_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -G "${GENERATOR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="${API}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DLSFGVK_ANDROID_WINE=ON \
    -DVOLK_STATIC_DEFINES=VK_USE_PLATFORM_ANDROID_KHR \
    -DCMAKE_CXX_FLAGS="-DVK_USE_PLATFORM_ANDROID_KHR" \
    -DCMAKE_C_FLAGS="-DVK_USE_PLATFORM_ANDROID_KHR"

cmake --build "${BUILD_DIR}" --parallel

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
STRIP_BIN="${ANDROID_NDK}/toolchains/llvm/prebuilt/${HOST_TAG:-${HOST_OS}-x86_64}/bin/llvm-strip"
if [[ -x "${STRIP_BIN}" ]]; then
    "${STRIP_BIN}" --strip-unneeded \
        -o "${DIST_DIR}/liblsfg-vk-${ABI}.so" \
        "${BUILD_DIR}/liblsfg-vk.so"
else
    echo "warning: llvm-strip not found at ${STRIP_BIN}; copying unstripped library" >&2
    cp "${BUILD_DIR}/liblsfg-vk.so" "${DIST_DIR}/liblsfg-vk-${ABI}.so"
fi

cp "${REPO_ROOT}/VkLayer_LS_frame_generation.json" "${BUILD_DIR}/"
cp "${REPO_ROOT}/VkLayer_LS_frame_generation.json" "${DIST_DIR}/"

echo ""
echo "Build complete. Artifacts:"
echo "  ${DIST_DIR}/liblsfg-vk-${ABI}.so"
echo "  ${DIST_DIR}/VkLayer_LS_frame_generation.json"
echo ""
echo "For GameNative Android app updates, copy the arm64-v8a shared library to:"
echo "  app/src/main/assets/lsfg_vk/android_arm64_v8a/liblsfg-vk-layer.so"
