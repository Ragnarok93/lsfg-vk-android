# Implementation Plan: Adaptive LSFG Unified Stability

## Overview

Consolidate the target-authoritative Adaptive LSFG scheduler from
`d472608f2412532a1c345efb5580d999ffd072bf` with the compatible presentation
capacity, diagnostics, and GameNative integration work from the later branch
lineage. The unified branches must retain source ownership of the real
timeline, keep deadline admission per-cycle, and make WSI capacity reductions
provisional and profitability-driven.

## Architecture decisions

- Target-authoritative scheduler policy remains the foundation. Source cadence
  determines interpolation demand but never causes long-term generation-cost
  backoff.
- The later source-protection scheduler state is deliberately excluded. Only
  the downstream presentation-capacity evidence/profitability behavior is
  ported.
- WSI capacity lowering is an experiment: preserve the prior higher cap and
  restore it when the lower cap does not materially improve useful delivery or
  efficiency.
- Deadline admission remains a per-cycle feasibility check and never mutates
  scheduler demand, source timing, or fractional phase.
- GameNative must use the committed LSFG submodule gitlink as the only native
  provenance source; branch names and remote heads are not build inputs.

## Assumptions

1. The explicit request authorizes creation and eventual publication of the
   two named development branches; remote branch deletion remains deferred
   until the audit and green verification gates complete.
2. Android device validation cannot be performed from this workspace unless a
   device/runner becomes available; the native and GameNative contract/build
   gates will be run and the gap will be reported explicitly.
3. Existing untracked files in the original B15 worktree are user-owned and
   must remain untouched.

## Task list

### Phase 1: Foundation and audit

- [x] Create isolated unified branches from the exact requested heads.
- [x] Audit the later LSFG commits and classify source-protection changes as
  rejected or presentation-capacity changes as compatible.
- [x] Audit GameNative changes unique to the Flow Scale line and exclude its
  obsolete LSFG pins.

### Phase 2: Native stability

- [x] Port target-aware, provisional WSI presentation-capacity tracking.
- [x] Thread output-deficit, deadline-capacity, and accepted-throughput context
  through the Android present path.
- [x] Extend native diagnostics without collapsing demand, admission, compute,
  presentation, Flow, and provenance into one pressure value.
- [x] Add regression coverage for target authority, fractional scheduling,
  per-cycle admission isolation, WSI profitability, Flow profitability, and
  fixed-mode isolation.

### Checkpoint: native

- [x] `adaptive_scheduler_test.cpp` passes.
- [x] `adaptive_flow_controller_test.cpp` passes.
- [x] All relevant Android contract and portability tests pass.
- [x] No source-preservation governor symbols or behavior are reintroduced.

### Phase 3: GameNative integration

- [x] Port the reviewed diagnostic exporter, manager, power-control metrics,
  and performance metrics changes.
- [x] Set the LSFG submodule gitlink to the final unified native SHA.
- [x] Validate exact SHA synchronization, native preparation, host tests,
  Android contract tests, and packaged artifact provenance.

### Checkpoint: candidate

- [ ] GameNative unit contracts pass (Gradle unavailable in this workspace).
- [ ] LegacyDebug APK assembles and contains the exact native marker/SHA.
- [ ] Device validation is performed only after all preceding gates pass.

### Phase 4: cleanup and handoff

- [ ] Review every superseded branch for unique useful code.
- [ ] Delete only branches proven redundant after unified branches are green.
- [ ] Report device-validation status and any remaining evidence gaps.

## Risks and mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Porting source-protection state accidentally lowers target demand | High | Keep scheduler policy on `d472608f`; add explicit no-backoff tests. |
| WSI cap remains an indirect target governor | High | Use provisional lower-cap evaluation and restore on throughput regression. |
| GameNative packages a different native revision | High | Verify gitlink SHA before build and inspect the APK after packaging. |
| Transport regression hidden by policy work | High | Preserve transport files and run AHB/SYNC_FD/nonblocking contracts. |
| Device evidence unavailable in workspace | Medium | Complete all deterministic gates and mark hardware validation unverified. |

## Deliberately rejected work

- Post-raise source-FPS vetoes, source-budget causal backoff, recovery
  thresholds tied to source rebound, source-preservation probes, retry timers,
  and any other logic whose purpose is to lower Adaptive generation density
  because source FPS declined.
- Wholesale merge/rebase of the later source-protection lineage.
- Deleting old remote branches before the audit and verification checkpoints.

## Audit record (2026-09-20)

- The native branch starts at `d472608f2412532a1c345efb5580d999ffd072bf`.
  The later source-protection line through `9575cc4e2ff8c3b26d5a9c289e5ba4576e8f8431`
  was reviewed without merging or rebasing. Its source-FPS veto/backoff,
  recovery-gated promotion, and source-preservation probes remain excluded.
- Only the later line's presentation-capacity evidence, accepted-throughput
  tracking, provisional lower-cap evaluation, profitability restore, and
  recoverable-duty behavior were recreated in native commits
  `1c632be`, `ce74545`, and `3cc293b`.
- GameNative starts from `e039a88135c056958eb96b857d6956681fb09bc2`.
  The diagnostic exporter, runtime manager, power sidecar, performance-metrics
  hook, and their tests were ported from the reviewed `6e0fc838...` state.
  Its obsolete `9575cc4e...` submodule pin was not retained; the integration
  branch now records the final unified native gitlink explicitly.
- Superseded remote branches remain untouched until the build/device gates are
  complete. Their deletion is a separate cleanup action, not part of the
  implementation commits.
