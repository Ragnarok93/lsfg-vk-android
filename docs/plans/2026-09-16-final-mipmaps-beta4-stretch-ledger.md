# Final Mipmaps + Beta4 Stretch Ledger

Date: 2026-09-16

Branch: `feature/adreno-6xx-candidate-b11-beta4-pow2-mask`

Starting native HEAD: `46e7d7952064acc39c4677284458336600891f4d`

Starting GameNative HEAD: `1cea4be160e181882b678a0ea9552cacee6692ff`

## Frozen observable requirements

- Preserve generated image quality and temporal behavior.
- Preserve adaptive generation, DeferredZero, pacing, suspend/resume, and
  enable/disable behavior.
- Do not add vendor, device, driver, or compiler-specific runtime branches.
- Reject average-time improvements that introduce long-tail stalls.
- Keep profiling and compiler dumps out of clean artifacts.

## Established baseline

Historical Adreno 650 evidence remains the comparison baseline:

- Mipmaps: approximately 5.222 ms.
- aggregate Beta: approximately 3.060 ms, dominated by `p_beta[4]`.
- Mipmaps + Beta: approximately 79.2% of measured generated-pass GPU work.
- Mipmaps IR3: approximately 617 executable instructions, 259 NOPs,
  18 shared stores, 29 shared loads, 83 SS stalls, 391 SY stalls, and eight
  resident waves.

The direct B12 stage reports remain evidence-only until an on-device capture
shows successful query readback. The portable analyzer and pipeline executable
capture remain the retention gates.

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

### Evidence still required

- Android arm64 and x86_64 clean builds.
- B11-versus-B13 `p_beta[4]` final executable comparison.
- On-device B13 application marker (`applied=1`).
- Repeated Beta4/aggregate timing and source/output FPS comparison.
- Image-quality and tail-latency review on Adreno 650.
- Portability run on Xclipse 940 or another non-Turnip backend.

## Mipmaps disposition

No Mipmaps mutation is retained yet. The prior B7 result proved that removing
65 private stores and promoting 143 private loads changed SPIR-V but did not
materially change the final executable or timing. Aggregate IR3 stall counts
do not identify which shared dependency can be removed safely. A barrier,
workgroup, subgroup, or topology rewrite without the exact producer/consumer
mapping would be speculative and could silently change reduction output.

The next Mipmaps candidate must start from one paired B12 + executable capture
and name a concrete target, such as an exact shared store/load dependency,
address chain, spill/reload, or barrier-adjacent live range. The user's expanded
engineering freedom supersedes the older notes that froze the five barriers and
32x32 topology; those structures may be replaced once a candidate includes an
independent output-equivalence check and multi-backend fallback/validation.

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

## Interruption checkpoint

Do not update GameNative's gitlink or package a clean APK until B13 has an
Android native commit and the clean native artifact contains the B13 marker.
Do not call B13 retained on performance grounds until final executable and
on-device evidence clear the gates above. Do not begin a Mipmaps topology
rewrite from aggregate counts alone; first preserve the exact instruction-level
capture that motivates it.
