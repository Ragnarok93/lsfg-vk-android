# Adaptive Flow Scale Auto Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an Adaptive Flow Scale `Auto` preset that searches the full 0.25–1.00 range in exact 0.05 increments, favors the highest sustainable scale while meeting the existing multiplier/adaptive frame-generation target, exposes complete transition telemetry, persists through GameNative, and appears in the requested 2x2 preset layout.

**Architecture:** Reuse the existing adaptive controller’s pressure, recovery, cooldown, and provisional-benefit policy. `Auto` supplies a descending 16-state lattice from 1.00 to 0.25; no new scheduler, pacing, WSI, or fractional-distribution policy is introduced. Native runtime contexts are prebuilt for every state, and the governor continues to request only prebuilt states. GameNative accepts, persists, hot-reloads, and displays the new preset. Native decision logs and periodic `LSFG_METRICS` records carry target/floor, state position, requested/active values, transition state, timing validity, target FPS, and pressure evidence.

**Tech Stack:** C++20 native controller and runtime, Android NDK/Vulkan layer, Kotlin/Compose GameNative settings and quick menu, TOML configuration, Python source-contract tests, C++ unit tests, Gradle tests, and GitHub Actions validation.

**Spec:** `docs/superpowers/specs/2026-09-28-adaptive-flow-auto-design.md`

## Global Constraints

- `Auto` is the only new preset. Existing `quality`, `balanced`, and `low` state arrays and behavior remain unchanged.
- The Auto lattice is exactly `1.00, 0.95, 0.90, 0.85, 0.80, 0.75, 0.70, 0.65, 0.60, 0.55, 0.50, 0.45, 0.40, 0.35, 0.30, 0.25`.
- Preserve the existing target multiplier/adaptive frame-generation coupling and existing controller policy; do not add a second governor.
- Keep runtime graph/context construction off the presentation path. Auto may increase prebuilt-context count, but must not introduce per-transition graph builds or destruction.
- Every state-change decision must expose enough information to distinguish target, floor, state index/count, requested scale, active scale, transition state, warmup, timing validity, target FPS, and pressure evidence.
- Preserve backward compatibility for omitted, empty, and existing preset values; unknown values continue to fail or sanitize according to the current native/GameNative boundary contract.
- Do not change unrelated synchronization, present pacing, WSI, scheduler, shader, or fractional-work behavior.

## Task 1: Add the native Auto state lattice and telemetry shape

**Files:**

- `include/adaptive_flow_controller.hpp`
- `src/adaptive_flow_controller.cpp`
- `tests/adaptive_flow_controller_test.cpp`

**Interfaces and behavior:**

- Add `AdaptiveFlowPreset::Auto`.
- Return the exact 16-state Auto lattice above from `statesForPreset(AdaptiveFlowPreset::Auto)` and expose its count in `AdaptiveFlowTelemetry` as `stateCount` alongside the existing `stateIndex`.
- Keep the current Quality, Balanced, and Low arrays byte-for-byte equivalent in values and order.
- Make preset naming/reason reporting recognize Auto without changing the existing adaptive decision policy.

**Implementation and verification steps:**

- [ ] Add failing controller tests first for the exact Auto values, endpoints, 0.05 adjacent spacing, descending order, state count, and full-range floor reachability.
- [ ] Implement the enum, lattice, preset name, state-count population, and any compile-time/static assertions needed to prevent accidental lattice drift.
- [ ] Run the controller’s C++20 `-Wall -Wextra -Werror` test command from `.github/workflows/adaptive-flow-validation.yml` and confirm existing preset behavior tests still pass.
- [ ] Commit as `feat: add full-range adaptive flow auto preset`.

## Task 2: Accept Auto in native configuration and add end-to-end native telemetry

**Files:**

- `include/config/config.hpp`
- `src/config/config.cpp`
- `include/context.hpp`
- `src/context.cpp`
- `tests/gamenative_adaptive_config_test.py`
- `tests/android_adaptive_flow_runtime_integration_test.py`
- `tests/android_adaptive_flow_shadow_transition_test.py`
- `tests/android_adaptive_history_test.py` (only where the telemetry contract is asserted)

**Interfaces and behavior:**

- Accept `adaptive_flow_preset = "auto"` in game TOML and legacy/environment configuration paths, with the existing quality default and existing compatibility behavior preserved.
- Map Auto through context initialization, prebuild all 16 Auto states using the existing adaptive-context path, and report the actual state count in initialization telemetry.
- Extend `AdaptiveFlowRuntimeSnapshot` as needed with state index/count and the active transition evidence required by logs.
- Extend every adaptive decision event with stable machine-readable fields for `preset`, `target`, `minimum`, `state_index`, `state_count`, `previous/requested/active`, `transition`, `warmup_remaining`, `timing_valid`, target FPS/multiplier evidence, and the existing flow/mipmap/total budget, generation, global GPU, compute pressure, WSI pressure/loss, output deficit, and output-satisfied evidence.
- Extend periodic text metrics and Android `LSFG_METRICS` output with the same target/floor/state/requested-vs-active contract, so validation can distinguish a requested transition from an applied transition.
- Preserve the existing decision cadence and downstep/upstep semantics; telemetry must observe the decision, not alter it.

**Implementation and verification steps:**

- [ ] Add failing Python source-contract assertions for Auto config acceptance, the 16-state initialization contract, and the required decision/periodic telemetry keys.
- [ ] Implement native config mapping and context snapshot propagation.
- [ ] Add one centralized formatting path or equivalent consistent field ordering for adaptive decision/metrics logs; keep Android and non-Android records semantically aligned.
- [ ] Run the native C++ controller test and the focused adaptive-flow Python tests, including runtime integration, shadow transition, history, and GameNative config validation.
- [ ] Commit as `feat: add adaptive flow auto config and telemetry`.

## Task 3: Add GameNative persistence, hot reload, and runtime version pinning

**Files:**

- `app/src/main/java/app/gamenative/utils/LsfgVkManager.kt`
- `app/src/main/java/app/gamenative/utils/LsfgQuickMenuHelper.kt`
- `app/src/test/java/app/gamenative/utils/LsfgVkManagerTest.kt`
- `app/src/test/java/app/gamenative/utils/LsfgQuickMenuHelperModeTest.kt`

**Interfaces and behavior:**

- Add `AUTO` constants and enum/mapping support at every existing GameNative adaptive-flow boundary.
- `sanitizeAdaptiveFlowPreset`, TOML serialization, runtime config updates, saved preferences, and hot-reload scheduling must round-trip `auto` without changing quality/balanced/low behavior.
- Retain the existing default behavior for missing/invalid persisted values.
- Update the runtime version/marker to the actual new native submodule commit after the native change is landed, so an installed runtime cannot be mistaken for the pre-Auto native build.

**Implementation and verification steps:**

- [ ] Add failing unit tests for Auto sanitization, serialization, runtime update mapping, and enum coverage.
- [ ] Implement the constants and mappings, then update the runtime marker using the actual native commit SHA rather than a placeholder.
- [ ] Run the focused GameNative unit tests and any existing adaptive config test suite.
- [ ] Commit as `feat: persist adaptive flow auto in gamenative`.

## Task 4: Implement the requested 2x2 preset UI and copy

**Files:**

- `app/src/main/java/app/gamenative/ui/component/QuickMenu.kt`
- `app/src/main/res/values/strings_lsfg_adaptive.xml`
- `app/src/test/java/app/gamenative/ui/component/LsfgAdaptiveFlowUiContractTest.kt`

**Interfaces and behavior:**

- Render adaptive presets as two equal-width rows in exactly this order:

  - top row: `Auto`, `Quality`
  - bottom row: `Low`, `Balanced`

- Keep the existing selected-state and interaction behavior.
- Add Auto copy that communicates automatic selection of the highest sustainable scale while meeting the target, with target 1.00 and minimum 0.25 consistent with the native contract.
- Keep the layout responsive without introducing horizontal overflow or changing fixed-flow controls.

**Implementation and verification steps:**

- [ ] Add failing UI contract assertions for Auto presence, exact row order, Auto description, and the existing preset descriptions.
- [ ] Implement the 2x2 Compose layout and Auto description branch.
- [ ] Run the focused UI contract/unit tests and the relevant GameNative Gradle test task.
- [ ] Commit as `feat: add adaptive flow auto quick menu preset`.

## Task 5: Pin the integrated native revision and perform repository validation

**Files:**

- GameNative gitlink `app/src/main/cpp/lsfg-vk-android`
- Any workflow or test files required only if validation identifies a missing contract

**Implementation and verification steps:**

- [ ] Update the GameNative gitlink to the native commit containing Tasks 1–2 using a normal tree/commit update; do not copy native source into GameNative.
- [ ] Confirm GameNative’s runtime marker and submodule SHA refer to the same native implementation.
- [ ] Run the native adaptive-flow validation workflow and inspect the actual run status/logs.
- [ ] Run the GameNative focused Gradle tests plus the repository’s relevant assemble/check task; if the repository builds the native layer from the submodule, verify the packaging path uses the new revision.
- [ ] Inspect both branch diffs for accidental changes outside the scoped files, stale test expectations, missing telemetry fields, or an unchanged native gitlink.
- [ ] Record confirmed tests, unavailable tests, and any build/artifact limitation in the final handoff.

## Review Focus

- **State lattice correctness:** exact 16 values, 0.05 spacing, descending order, endpoints, and no regressions in existing preset arrays. Owned by Task 1 controller tests.
- **Policy behavior:** Auto reuses existing pressure/recovery logic, can reach 0.25 under sustained pressure, and still favors recovery to the highest sustainable state. Owned by Task 1 behavior tests and Task 2 runtime contracts.
- **Configuration compatibility:** TOML, legacy/environment, saved preference, serialization, and hot reload all accept Auto while preserving defaults and existing presets. Owned by Task 2 and Task 3 tests.
- **Observability correctness:** decision and periodic metrics include target/floor, state index/count, requested-vs-active values, transition/warmup/timing status, target FPS, and pressure evidence. Owned by Task 2 Python contracts and log inspection.
- **Integration correctness:** UI order is Auto/Quality over Low/Balanced, selected state persists, and GameNative points at the native commit that implements the feature. Owned by Task 4 UI tests and Task 5 gitlink/version checks.

---

Execution should proceed task-by-task with a test-first change, focused verification, and a separate commit for each task. The native controller and telemetry work must land before the GameNative gitlink and runtime marker are updated.