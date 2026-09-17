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
#   LSFGVK_B12_DUAL_STAGE_PROFILE=1 LSFGVK_ADAPTIVE_RUNTIME=1 ... # lightweight Mipmaps + Beta4 timing
#   LSFGVK_MIPMAPS_EXEC_PROFILE=1 LSFGVK_B12_DUAL_STAGE_PROFILE=1 LSFGVK_ADAPTIVE_RUNTIME=1 ... # optional compiler executable/IR evidence
#   LSFGVK_MIPMAPS_CANDIDATE_SCRIPT=scripts/apply-my-mipmaps-candidate.py LSFGVK_B12_DUAL_STAGE_PROFILE=1 ... # checked device-agnostic candidate
#   LSFGVK_ZERO_STAGE_PROFILE=1 ... ./scripts/build/android.sh     # profiling build
#   LSFGVK_B11_EVIDENCE_PROFILE=1 LSFGVK_B11_PROFILE_VARIANT=b4 ...  # B4-only controlled evidence
#   LSFGVK_B11_EVIDENCE_PROFILE=1 LSFGVK_B11_PROFILE_VARIANT=b11 ... # B4+B11 controlled evidence
#   LSFGVK_B11_EVIDENCE_PROFILE=1 LSFGVK_B11_PROFILE_VARIANT=b13 ... # B4+B11+B13 controlled evidence
#   LSFGVK_ZERO_STAGE_PROFILE=1 LSFGVK_B8_DIAGNOSTICS=1 ...       # legacy B8 diagnostic matrix
#   LSFGVK_ZERO_STAGE_PROFILE=1 LSFGVK_FINAL_NONADAPTIVE_SWEEP=1 ... # deferred final sweep
#   LSFGVK_EXPERIMENTAL_B9=1 ... ./scripts/build/android.sh        # experimental Beta4 scheduler

set -euo pipefail

BUILD_TYPE="${1:-Release}"
ABI="${ANDROID_ABI:-arm64-v8a}"
API="${ANDROID_PLATFORM:-android-28}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
B11_EVIDENCE_PROFILE="${LSFGVK_B11_EVIDENCE_PROFILE:-0}"
B11_PROFILE_VARIANT="${LSFGVK_B11_PROFILE_VARIANT:-b11}"
B12_DUAL_STAGE_PROFILE="${LSFGVK_B12_DUAL_STAGE_PROFILE:-0}"
MIPMAPS_EXEC_PROFILE="${LSFGVK_MIPMAPS_EXEC_PROFILE:-${LSFGVK_B12_MIPMAPS_EXEC_PROFILE:-0}}"
MIPMAPS_CANDIDATE_SCRIPT="${LSFGVK_MIPMAPS_CANDIDATE_SCRIPT:-}"
ADAPTIVE_RUNTIME="${LSFGVK_ADAPTIVE_RUNTIME:-0}"
EXPERIMENTAL_B9="${LSFGVK_EXPERIMENTAL_B9:-0}"

if [[ -z "${ANDROID_NDK:-}" ]]; then
    echo "error: ANDROID_NDK must be set to your NDK root (e.g. /opt/android-ndk-r27d)" >&2
    exit 1
fi

if [[ "${B12_DUAL_STAGE_PROFILE}" != "0" && "${B12_DUAL_STAGE_PROFILE}" != "1" ]]; then
    echo "error: LSFGVK_B12_DUAL_STAGE_PROFILE must be 0 or 1" >&2
    exit 1
fi
if [[ "${MIPMAPS_EXEC_PROFILE}" != "0" && "${MIPMAPS_EXEC_PROFILE}" != "1" ]]; then
    echo "error: LSFGVK_MIPMAPS_EXEC_PROFILE must be 0 or 1" >&2
    exit 1
fi
if [[ "${MIPMAPS_EXEC_PROFILE}" == "1" && "${B12_DUAL_STAGE_PROFILE}" != "1" ]]; then
    echo "error: LSFGVK_MIPMAPS_EXEC_PROFILE requires LSFGVK_B12_DUAL_STAGE_PROFILE=1" >&2
    exit 1
fi
if [[ -n "${MIPMAPS_CANDIDATE_SCRIPT}" && "${B12_DUAL_STAGE_PROFILE}" != "1" ]]; then
    echo "error: LSFGVK_MIPMAPS_CANDIDATE_SCRIPT requires LSFGVK_B12_DUAL_STAGE_PROFILE=1" >&2
    exit 1
fi

if [[ "${B11_EVIDENCE_PROFILE}" == "1" ]]; then
    if [[ "${B11_PROFILE_VARIANT}" != "b4" && "${B11_PROFILE_VARIANT}" != "b11" \
            && "${B11_PROFILE_VARIANT}" != "b13" ]]; then
        echo "error: LSFGVK_B11_PROFILE_VARIANT must be b4, b11, or b13" >&2
        exit 1
    fi
    if [[ "${ADAPTIVE_RUNTIME}" == "1" ]]; then
        echo "error: B11 evidence profiling cannot be combined with clean adaptive runtime mode" >&2
        exit 1
    fi
    if [[ "${LSFGVK_B8_DIAGNOSTICS:-0}" == "1" || "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]; then
        echo "error: B11 evidence profiling cannot be combined with B8/final compiler sweeps" >&2
        exit 1
    fi
    if [[ "${EXPERIMENTAL_B9}" == "1" ]]; then
        echo "error: B11 evidence profiling cannot be combined with experimental B9" >&2
        exit 1
    fi
fi

if [[ "${B12_DUAL_STAGE_PROFILE}" == "1" ]]; then
    if [[ "${B11_EVIDENCE_PROFILE}" == "1" || "${LSFGVK_ZERO_STAGE_PROFILE:-0}" == "1" ]]; then
        echo "error: B12 dual-stage profiling cannot be combined with the heavyweight evidence profilers" >&2
        exit 1
    fi
    if [[ "${LSFGVK_B8_DIAGNOSTICS:-0}" == "1" || "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]; then
        echo "error: B12 dual-stage profiling cannot be combined with B8/final compiler sweeps" >&2
        exit 1
    fi
    if [[ "${EXPERIMENTAL_B9}" == "1" ]]; then
        echo "error: B12 dual-stage profiling cannot be combined with experimental B9" >&2
        exit 1
    fi
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

PROFILE_REQUESTED="${LSFGVK_ZERO_STAGE_PROFILE:-0}"
if [[ "${B11_EVIDENCE_PROFILE}" == "1" ]]; then
    PROFILE_REQUESTED=1
fi

if [[ "${PROFILE_REQUESTED}" == "1" ]]; then
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
        if [[ "${B11_EVIDENCE_PROFILE}" == "1" ]]; then
            echo "[lsfg-vk] Enabling direct Beta4 timing and pipeline executable evidence (${B11_PROFILE_VARIANT})"
            python3 "${REPO_ROOT}/scripts/apply-b11-beta4-stage-profile.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-b11-beta4-executable-profile.py" --root "${REPO_ROOT}"
        fi
        if [[ "${LSFGVK_FINAL_NONADAPTIVE_SWEEP:-0}" == "1" ]]; then
            echo "[lsfg-vk] Enabling deferred final non-adaptive compiler sweep"
            if [[ "${B11_EVIDENCE_PROFILE}" != "1" ]]; then
                python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
            fi
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-local-spirv-constants.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-final-nonadaptive-sweep-v2.py" --root "${REPO_ROOT}"
        elif [[ "${LSFGVK_B8_DIAGNOSTICS:-0}" == "1" ]]; then
            echo "[lsfg-vk] Enabling opt-in B8 pipeline-executable and mipmaps-matrix diagnostics"
            if [[ "${B11_EVIDENCE_PROFILE}" != "1" ]]; then
                python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
            fi
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-mipmaps-matrix.py" --root "${REPO_ROOT}"
            python3 "${REPO_ROOT}/scripts/apply-candidate-b8-local-spirv-constants.py" --root "${REPO_ROOT}"
        fi
    fi
fi

python3 "${REPO_ROOT}/scripts/apply-candidate-b-translation-cleanup.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/apply-candidate-b4-beta4-predicate.py" --root "${REPO_ROOT}"
if [[ "${B11_EVIDENCE_PROFILE}" == "1" && "${B11_PROFILE_VARIANT}" == "b4" ]]; then
    echo "[lsfg-vk] Controlled B11 evidence variant: retaining B4 without B11"
else
    python3 "${REPO_ROOT}/scripts/apply-candidate-b11-beta4-pow2-mask.py" --root "${REPO_ROOT}"
    if [[ "${B11_EVIDENCE_PROFILE}" == "1" && "${B11_PROFILE_VARIANT}" == "b11" ]]; then
        echo "[lsfg-vk] Controlled B13 evidence variant: retaining B4+B11 without B13"
    else
        python3 "${REPO_ROOT}/scripts/apply-candidate-b13-beta4-fused-mask.py" --root "${REPO_ROOT}"
    fi
fi
if [[ "${LSFGVK_EXPERIMENTAL_B9:-0}" == "1" ]]; then
    echo "[lsfg-vk] Enabling experimental B9 Beta4 scheduler"
    python3 "${REPO_ROOT}/scripts/apply-candidate-b9-beta4-spill-collapse.py" --root "${REPO_ROOT}"
fi

if [[ -n "${MIPMAPS_CANDIDATE_SCRIPT}" ]]; then
    if [[ "${MIPMAPS_CANDIDATE_SCRIPT}" = /* ]]; then
        candidate_path="${MIPMAPS_CANDIDATE_SCRIPT}"
    else
        candidate_path="${REPO_ROOT}/${MIPMAPS_CANDIDATE_SCRIPT}"
    fi
    if [[ ! -f "${candidate_path}" ]]; then
        echo "error: Mipmaps candidate script not found: ${candidate_path}" >&2
        exit 1
    fi
    python3 "${REPO_ROOT}/scripts/check-mipmaps-device-agnostic.py" "${candidate_path}"
    echo "[lsfg-vk] Applying checked device-agnostic Mipmaps candidate ${candidate_path}"
    python3 "${candidate_path}" --root "${REPO_ROOT}"
fi

# Apply after all optional source transforms so the Android CPU hot-path
# transforms cannot invalidate their source anchors.
python3 "${REPO_ROOT}/scripts/apply-android-command-buffer-reuse.py" --root "${REPO_ROOT}"
python3 "${REPO_ROOT}/scripts/apply-android-submit-hot-path.py" --root "${REPO_ROOT}"

if [[ "${B12_DUAL_STAGE_PROFILE}" == "1" ]]; then
    echo "[lsfg-vk] Enabling B12 low-overhead Mipmaps + Beta4 GPU timing"
    python3 "${REPO_ROOT}/scripts/apply-b12-dual-stage-profile.py" --root "${REPO_ROOT}"
    python3 "${REPO_ROOT}/scripts/apply-b12-unreported-timestamp-fallback.py" --root "${REPO_ROOT}"
    python3 "${REPO_ROOT}/scripts/apply-b12-reporting-hardening.py" --root "${REPO_ROOT}"
    python3 "${REPO_ROOT}/scripts/apply-b12-device-profile.py" --root "${REPO_ROOT}"
    if [[ "${MIPMAPS_EXEC_PROFILE}" == "1" ]]; then
        echo "[lsfg-vk] Enabling optional p_mipmaps pipeline executable/IR capture"
        python3 "${REPO_ROOT}/scripts/apply-candidate-b6-pipeline-executable-profile.py" --root "${REPO_ROOT}"
    fi
fi

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
