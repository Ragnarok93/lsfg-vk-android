# Device-Agnostic Mipmaps Evidence and Refinement Implementation Plan

> **For agentic workers:** Use the host's available task-by-task implementation workflow. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make future Mipmaps refinement structurally device agnostic while preserving backend-specific compiler evidence as optional diagnostics.

**Architecture:** B12 remains the portable runtime timing source. `scripts/analyze-mipmaps-evidence.py` owns generic timing, device identity, executable hashing, repeated-baseline noise modeling, and candidate verdicts; compiler-specific parsing lives behind optional adapters beginning with IR3. Candidate shader transforms enter through a checked build interface that rejects GPU/vendor/driver identity branching before the transform is applied.

**Tech Stack:** Python 3, Bash, Vulkan pipeline executable properties, GitHub Actions, Android NDK/CMake.

## Global Constraints

- Mipmaps algorithm semantics remain unchanged unless separately justified: 32x32 workgroup, five barriers, 1024 -> 256 -> 64 -> 16 -> 4 -> 1 reduction order, FP evaluation order, samples, writes, formats, and image output.
- A retained Mipmaps optimization must not branch on GPU vendor, device model, driver identity, or compiler backend.
- Backend-specific executable evidence is supporting evidence only; unsupported compilers must not prevent portable timing analysis.
- Performance decisions use repeated same-device baselines. A single baseline cannot establish the noise model for the generic retention gate.
- The generic gate combines measured baseline variance with percentage change. Historical Adreno absolute bands remain available only through the compatibility analyzer for historical comparisons.
- Ordinary gameplay evidence enables B12 timing and portable device metadata only. Verbose executable/IR capture remains opt-in.
- Clean production builds remain free of all B12 evidence instrumentation.

---

### Task 1: Portable evidence core and optional compiler adapter

**Files:**
- Create: `scripts/mipmaps_evidence/core.py`
- Create: `scripts/mipmaps_evidence/backends/ir3.py`
- Create: `scripts/analyze-mipmaps-evidence.py`
- Modify: `scripts/analyze-adreno-mipmaps-evidence.py`
- Test: `tests/android_mipmaps_device_agnostic_evidence_test.py`
- Test: `tests/android_mipmaps_evidence_analyzer_test.py`

**Interfaces:**
- Consumes: B12 `b12-stage-profile`, `b12-device-profile`, and optional `pipeline-exec-*` log records.
- Produces: JSON/text reports containing portable timing/device/executable evidence plus optional backend metrics.

- [x] Add contracts for unsupported-backend tolerance, repeated-baseline variance, IR3 compatibility, and historical wrapper behavior.
- [x] Implement portable parsing and sample-weighted timing.
- [x] Implement repeated same-device baseline noise model and percentage-based decision gate.
- [x] Move IR3-only load/store/stall/register/wave parsing into an optional adapter.
- [x] Preserve the historical Adreno CLI schema and historical absolute thresholds for old evidence comparisons only.

### Task 2: Enforce device-agnostic candidate transforms

**Files:**
- Create: `scripts/check-mipmaps-device-agnostic.py`
- Modify: `scripts/build/android.sh`
- Test: `tests/android_mipmaps_device_agnostic_evidence_test.py`

**Interfaces:**
- Consumes: `LSFGVK_MIPMAPS_CANDIDATE_SCRIPT=<repo-relative-or-absolute-script>`.
- Produces: fail-closed validation followed by candidate transform execution only when no device/vendor/driver/backend identity token is present.

- [x] Reject vendor/device/driver identity branching and named GPU/compiler backend coupling.
- [x] Add the checked candidate-script build interface.
- [x] Add generic `LSFGVK_MIPMAPS_EXEC_PROFILE=1`; retain `LSFGVK_B12_MIPMAPS_EXEC_PROFILE` as a compatibility alias.
- [x] Require B12 timing mode for refinement candidates and executable capture.

### Task 3: Portable runtime device metadata

**Files:**
- Create: `scripts/apply-b12-device-profile.py`
- Modify: `scripts/build/android.sh`
- Test: `tests/android_mipmaps_device_agnostic_evidence_test.py`

**Interfaces:**
- Produces once per evidence runtime: `b12-device-profile vendor_id=... device_id=... device_name=... driver_version=... api_version=... timestamp_period_ns=... compute_family=...`.

- [x] Derive metadata exclusively from Vulkan physical-device properties and the already-selected compute family.
- [x] Apply only in B12 evidence builds after timestamp capability instrumentation.
- [x] Keep clean runtime binaries free of the marker.

### Task 4: CI and packaging integration

**Files:**
- Modify: `.github/workflows/android-bionic.yml`
- Modify later in GameNative: `.github/workflows/lsfg-legacy-single-apk.yml`

**Interfaces:**
- CI runs both portable and compatibility analyzer contracts; B12 artifact audits require portable metadata; clean artifact audits forbid it.
- GameNative ordinary evidence APK keeps verbose executable capture disabled.

- [ ] Verify the complete LSFG policy suite, clean arm64/x86 builds, B12 timing build, and B12+executable evidence build.
- [ ] Pin the verified LSFG head into GameNative.
- [ ] Verify GameNative Legacy tests, APK assembly, packaged native-layer marker audit, and artifact upload.

## Acceptance

A future Mipmaps candidate may be authored using IR3 or another backend as a microscope, but the transform itself must remain backend-neutral. Retention requires preserved shader semantics, statistically meaningful same-device timing improvement beyond that device's measured baseline variance, no image-quality/frame-pacing regression, and no material regression on other tested architectures. Backend-specific executable metrics may reject a candidate when available (for example a new spill or occupancy loss), but absence of such metrics never blocks portable timing analysis.
