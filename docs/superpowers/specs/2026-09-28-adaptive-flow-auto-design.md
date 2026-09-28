# Adaptive Flow Scale Auto Mode

## Status

Proposed

## Date

2026-09-28

## Context

GameNative already exposes Fixed and Adaptive Flow Scale modes. Adaptive mode currently selects one of three quality envelopes:

- Quality: 1.00 down to 0.70
- Balanced: 0.80 down to 0.55
- Low: 0.55 down to 0.25

The native controller already consumes completed LSFG GPU timing, frame-budget telemetry, output-cadence deficit, downstream presentation pressure, and GameNative's adaptive-frame-generation target. It begins at the selected preset target, lowers Flow Scale only after sustained material pressure, evaluates whether a downstep produced useful relief, and recovers quality slowly when the higher state is predicted to fit.

The missing capability is an Auto envelope that can search the entire supported Flow Scale range at the requested 0.05 resolution. The missing validation surface is complete telemetry for target/floor, requested/active state, transitions, and the evidence behind each controller decision.

## Goals

1. Add an Auto Adaptive Flow Scale preset spanning 1.00 through 0.25 in 0.05 increments.
2. Bias Auto toward the highest Flow Scale that can sustain the configured fixed multiplier or Adaptive Frame Generation output target under real load.
3. Preserve the existing source timeline, adaptive frame-generation scheduler, WSI/presentation policy, AHardwareBuffer transport, and Adreno/Xclipse synchronization behavior.
4. Expose Auto beside the existing presets in this exact 2×2 order:
   - top row: Auto, Quality
   - bottom row: Low, Balanced
5. Persist, serialize, hot-reload, and validate the new preset.
6. Add enough native logging/Android logcat telemetry to reconstruct every Auto scale transition and determine whether it met the requested output target.

## Non-goals

- Do not change source-frame pacing.
- Do not change the adaptive multiplier scheduler or fractional opportunity distribution.
- Do not change WSI present-mode behavior, semaphore ownership, AHardwareBuffer handoff, or frame-retirement policy.
- Do not add vendor-specific branches.
- Do not replace the existing validated controller with a new independent governor.
- Do not silently substitute a coarser state lattice if the 0.05 requirement causes a measurable memory or startup problem; report that constraint for a separate decision.

## Decision

### Native preset and state lattice

Add AdaptiveFlowPreset::Auto to the native controller and map the config string 'auto' to it.

Auto uses these descending states:

```text
1.00, 0.95, 0.90, 0.85, 0.80, 0.75, 0.70, 0.65,
0.60, 0.55, 0.50, 0.45, 0.40, 0.35, 0.30, 0.25
```

The controller target is 1.00 and the hard floor is 0.25. Quality, Balanced, and Low retain their existing target/floor envelopes while using exact 0.05 state spacing:

- Quality: 1.00, 0.95, 0.90, 0.85, 0.80, 0.75, 0.70
- Balanced: 0.80, 0.75, 0.70, 0.65, 0.60, 0.55
- Low: 0.55, 0.50, 0.45, 0.40, 0.35, 0.30, 0.25

The Android adaptive context will prebuild the complete Auto graph set using the existing prepared-state handoff mechanism. Runtime changes remain requests to already-built contexts; no graph is constructed or destroyed on the present thread.

### Control policy

Reuse the existing controller policy:

- Start at the envelope target.
- Treat completed GPU timing and target/output deficit as authoritative evidence.
- Lower only after sustained pressure and a material predicted contribution from Flow Scale.
- Make each lower state provisional until output, source cadence, compute timing, presentation pressure, or global GPU pressure demonstrates measurable benefit.
- Hold/revert a downstep when it does not improve the constrained workload.
- Raise one state at a time only after sustained headroom and a predicted higher-state budget fit.
- Never use source-rate degradation alone as permission to lower quality or to pace the source.

Because the existing frame budget and output-deficit inputs already account for fixed-multiplier and Adaptive Frame Generation operation, Auto will pursue the configured multiplier/output-FPS target without adding another target governor.

### Configuration and UI

Accept 'auto' in:

- native TOML validation and config mapping;
- legacy environment parsing;
- GameNative preset constants, sanitization, persistence, and hot reload;
- diagnostic/static contract tests.

The Quick Menu will render two equal-width rows:

```text
[ Auto ]    [ Quality ]
[ Low  ]    [ Balanced ]
```

Auto copy will state that it seeks the highest sustainable Flow Scale across the full 1.00–0.25 range while honoring the requested output target.

### Telemetry contract

Keep transition logging event-based and periodic metrics logging separate.

Every adaptive Flow decision that changes the requested state must expose:

- runtime session and config revision;
- preset name;
- target and minimum Flow Scale;
- state index and total state count;
- previous, requested, and active Flow Scale when available;
- transition-pending and warmup state;
- decision reason;
- frame budget, mipmaps, Flow, and total LSFG duration;
- generation count and whether the sample contained generated work;
- source/output FPS and output-target satisfaction/deficit;
- global GPU pressure validity/value;
- compute pressure, WSI pressure, WSI loss rate, and predicted next total.

Periodic LSFG_METRICS Android logcat records must additionally include the controller target/floor, requested/active values, state index/count, transition/warmup state, timing validity, and decision reason. Existing text diagnostics must retain the same fields so GameNative diagnostic exports can validate the run without requiring a new file format.

Initialization telemetry must identify preset=auto, target=1.000, minimum=0.250, and states=16.

## Files in scope

### lsfg-vk-android

- include/adaptive_flow_controller.hpp
- src/adaptive_flow_controller.cpp
- include/config/config.hpp / src/config/config.cpp as required for validation
- include/context.hpp / src/context.cpp for mapping, state telemetry, and logs
- adaptive controller tests and native/config contract tests

### GameNative

- LsfgVkManager.kt
- LsfgQuickMenuHelper.kt
- QuickMenu.kt
- strings_lsfg_adaptive.xml
- focused helper, manager, and UI contract tests
- the native submodule reference after the lsfg-vk-android commit is available

## Verification plan

1. Compile and run the native adaptive controller tests with warnings treated as errors.
2. Verify Auto state ordering, exact 0.05 spacing, endpoints, pressure downsteps, recovery, and floor behavior.
3. Run native Python config/runtime contract tests.
4. Run GameNative Kotlin unit/contract tests for enum membership, sanitization, serialization, persistence, and exact 2×2 UI order.
5. Inspect the integrated diff and confirm no source pacing, WSI, synchronization, or transport files changed outside the telemetry/config seams.
6. Run repository CI/build validation.
7. Validate runtime logs on both the Galaxy S20+ Adreno 650 and Galaxy S25 FE Xclipse 940. Confirm:
   - Auto starts at 1.00;
   - state changes occur in 0.05 steps;
   - target/floor/requested/active telemetry is present;
   - lower states are retained only when they improve target/pressure evidence;
   - the existing stable presentation behavior is unchanged.

## Acceptance criteria

- Auto is selectable and persisted.
- Auto serializes as adaptive_flow_preset = "auto".
- Auto initializes at 1.00 and can reach 0.25 through exact 0.05 states.
- Auto remains biased toward the highest sustainable state.
- Existing Quality, Balanced, and Low target/floor envelopes and controller policy are unchanged; their state spacing is exact 0.05.
- The UI order is exactly Auto/Quality over Low/Balanced.
- Transition and periodic telemetry are sufficient to determine why every state changed.
- Native and GameNative focused tests pass.
- No known-good Adreno or Xclipse synchronization/presentation path is modified.