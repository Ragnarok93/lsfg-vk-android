# Final Mipmaps + Beta4 Stretch Ledger

Date: 2026-09-16

Branch: `feature/adreno-6xx-candidate-b11-beta4-pow2-mask`

Starting native HEAD: `46e7d7952064acc39c4677284458336600891f4d`

Starting GameNative HEAD: `1cea4be160e181882b678a0ea9552cacee6692ff`

Retained B13 native baseline: `6d698f8` (`perf(beta4): fuse and hoist
reduction masks`)

Retained B13 GameNative pin: `982a0386` (`build(lsfg): pin Beta4 fused-mask
candidate`)

## Frozen observable requirements

- Preserve generated image quality and temporal behavior.
- Preserve adaptive generation, DeferredZero, pacing, suspend/resume, and
  enable/disable behavior.
- Do not add vendor, device, driver, or compiler-specific runtime branches.
- Reject average-time improvements that introduce long-tail stalls.
- Keep profiling and compiler dumps out of clean artifacts.

## Historical baseline

Historical Adreno 650 evidence remains the comparison baseline:

- Mipmaps: approximately 5.222 ms.
- aggregate Beta: approximately 3.060 ms, dominated by `p_beta[4]`.
- Mipmaps + Beta: approximately 79.2% of measured generated-pass GPU work.
- Mipmaps IR3: approximately 617 executable instructions, 259 NOPs,
  18 shared stores, 29 shared loads, 83 SS stalls, 391 SY stalls, and eight
  resident waves.

The direct B12 stage reports remain evidence-only where query readback does not
meet its validity contract. The portable analyzer, final pipeline executable,
aggregate runtime behavior, and matched device captures remain retention
evidence.

## Experiment B13: fused, hoisted Beta4 masks

### Bottleneck hypothesis

B11 proves that all five Beta4 reduction steps are powers of two, but the
generated SPIR-V still reconstructs each 2D lane predicate independently. The
B11 form emits, per phase, two masks, two zero comparisons, and a logical AND;
B4/B11 also reload and extract the same local invocation coordinates in every
phase.

For unsigned coordinates and `mask = step - 1`:

```text
(x & mask) == 0 && (y & mask) == 0
    == ((x | y) & mask) == 0
```

### Candidate

`scripts/apply-candidate-b13-beta4-fused-mask.py` runs after B4+B11 and:

- emits LocalInvocationId, x, y, and `x | y` once before the first reduction
  selection;
- reuses the dominating combined coordinate in all five later selections;
- emits one mask and one zero comparison per phase;
- leaves the B4 modulo construction intact as the fail-closed fallback when
  B11 cannot prove the power-of-two contract;
- changes no sample, floating-point expression, shared-memory operation,
  barrier, workgroup dimension, image operation, descriptor, format, runtime
  synchronization, scheduler, or presentation code.

The proven B11 path falls from 40 predicate-construction SPIR-V operations/IDs
to 14: four shared coordinate operations and two operations for each of five
phases. This is 26 fewer operations/IDs before backend optimization. Retention
still depends on a changed final executable and repeatable device performance;
this source-level reduction is not itself a performance claim.

### Evidence completed

- Exhaustive identity check for x/y in `[0, 255]` and masks for steps
  2, 4, 8, 16, and 32.
- Transform composition and idempotence check over B4 -> B11 -> B13.
- Fail-closed B4 fallback retained in generated C++.
- Host `g++ -fsyntax-only` check of the fully composed translation unit passed.
- Full Android Python contract suite passed: 115 tests.

### Retention result

B13 is the retained runtime baseline. In the matched sustained 4x comparison,
pooled B11-to-B13 results showed source FPS +6.05%, output FPS +4.36%, cycle
time -11.11%, GPU wait -11.6%, and dispatch time -7.69%. The exact-parent
B11-to-B13 comparison showed source FPS +4.4%, output FPS +2.9%, cycle time
-11.3%, and GPU wait -12.1%. No present failure, AHardwareBuffer fallback,
adaptive, or DeferredZero regression was observed in the accepted captures.

These results establish B13, rather than B11, as the control for all subsequent
Mipmaps work.

## Mipmaps B14 starting point

The current B13 Mipmaps observation is approximately 4.00 ms under the matched
device procedure. The B14 retention target is at most 3.20 ms, a 20% reduction.

Instruction-level evidence now localizes a concrete dependency. The 16-to-4
phase stores four reduced values to workgroup memory, all 1024 lanes execute a
fifth workgroup barrier, and lane zero reloads those same four values for the
4-to-1 result. B14 replaces that final rendezvous with one lane computing the
same four values in registers, writing the same mip5 texels, and applying the
same final reduction tree to write mip6. It preserves the exact FP32 grouping
and targets four shared stores plus one whole-workgroup barrier.

The approved design and implementation plan are recorded in:

- `docs/plans/2026-09-16-b14-mipmaps-tail-fusion-design.md`
- `docs/superpowers/plans/2026-09-16-b14-mipmaps-tail-fusion.md`

### B14 host evidence completed

- Exact B13 translated-shader fixture: 28,832 bytes, 7,208 words, bound 1270,
  SHA-256 `68c68ffd7308d0cc742aa3e9ecbd00f44c92893c23df5cd62e1318cdb75b9046`.
- B14 translated shader: 26,708 bytes, 6,677 words, bound 1383,
  SHA-256 `c0c947bfb2314b098e729c61622b791a13b9110cec5aaf6a03ad983888ccf4a2`.
- Parsed SPIR-V instructions: 1,508 to 1,385 (-123 / -8.2%).
- Workgroup barriers: five to four.
- Dynamic tail workgroup stores: four to zero.
- Dynamic tail workgroup loads: fifteen to fifteen.
- Static workgroup loads: fifteen to twenty-four because the lane-zero tail
  spells out the fifteen dynamic loads instead of sharing three load opcodes
  across active lanes.
- Logical image outputs remain ten; static `OpImageWrite` opcodes rise from
  ten to thirteen because mip5 now has four explicit lane-zero writes.
- The real C++ rewriter rejects mutated baseline and candidate modules without
  changing them, and exact second application is idempotent.
- `spirv-val --target-env vulkan1.3` accepts the transformed SPIR-V 1.6 module.
- The composed cleanup -> B4 -> B11 -> B13 -> B14 translation unit passes host
  C++20 syntax compilation.
- The complete Android policy suite passes: 118 tests.

Android arm64-v8a/x86_64 builds, final backend executable evidence, and device
timing/quality/stability remain required. The current host evidence proves a
valid candidate, not the 3.20 ms performance target.

## Reproduction commands

Policy suite:

```sh
python3 -m unittest discover -s tests -p 'android_*_test.py'
```

B13 controlled executable/timing evidence build:

```sh
LSFGVK_B11_EVIDENCE_PROFILE=1 \
LSFGVK_B11_PROFILE_VARIANT=b13 \
ANDROID_NDK=/path/to/android-ndk-r27d \
./scripts/build/android.sh Release
```

B11 control uses the same command with `LSFGVK_B11_PROFILE_VARIANT=b11`.
Compare repeated B11 controls with B13 using final `p_beta[4]` executable
evidence and B12 timing:

```sh
python3 scripts/analyze-beta4-evidence.py \
  --baseline b11-run-1.log \
  --baseline b11-run-2.log \
  --candidate b13-run.log \
  --json
```

Portable Mipmaps refinement evidence build:

```sh
LSFGVK_ADAPTIVE_RUNTIME=1 \
LSFGVK_B12_DUAL_STAGE_PROFILE=1 \
LSFGVK_MIPMAPS_EXEC_PROFILE=1 \
ANDROID_NDK=/path/to/android-ndk-r27d \
./scripts/build/android.sh Release
```

B14 uses the same evidence build with:

```sh
LSFGVK_MIPMAPS_CANDIDATE_SCRIPT=scripts/apply-candidate-b14-mipmaps-tail-fusion.py
```

## Interruption checkpoint

B13 is retained and pinned. B14 is evidence-only until its structural,
executable, timing, quality, stability, and second-backend gates pass. Do not
update GameNative's gitlink for B14 or package B14 as a clean deliverable before
that retention decision.
