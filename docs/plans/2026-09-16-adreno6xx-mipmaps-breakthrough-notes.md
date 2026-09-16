# Adreno 6xx Mipmaps Breakthrough Notes

Date: 2026-09-16

## Purpose

Preserve the strongest evidence from the completed Adreno 6xx optimization probes without turning the current pass into another speculative Mipmaps rewrite. These notes define the only promising follow-up directions that remain compatible with the campaign's stability and image-quality constraints.

## Current evidence

At flow scale 0.50 the measured generated-pass pipeline is approximately 10.463 ms:

- Mipmaps: 5.222 ms (49.9%)
- Beta: 3.060 ms (29.2%)
- Everything else: 2.181 ms (20.8%)

Mipmaps + Beta therefore account for about 79.2% of measured generated-pass GPU cost.

The observed Mipmaps shift from the original run to B4 (+0.035 ms / +0.68%) is not evidence of a regression. The samples overlap and the B4 capture ran under substantially higher generation pressure, so DVFS, saturation, cache residency, and thermal state are confounders.

The B6 executable evidence for Mipmaps showed approximately:

- 617 executable instructions
- 259 NOPs
- 18 shared-memory stores
- 29 shared-memory loads
- 83 SS stalls
- 391 SY stalls
- 8 resident waves

B7 separately demonstrated that shrinking SPIR-V from roughly 1,508 to 1,444 instructions did not materially change the executable or timing. SPIR-V word/instruction reduction by itself is therefore not a retention criterion.

## Hard invariants

Any future Mipmaps candidate must preserve all of the following unless new evidence explicitly justifies revisiting the campaign constraints:

- 32x32 workgroup
- all five reduction barriers
- exact 1024 -> 256 -> 64 -> 16 -> 4 -> 1 reduction order
- floating-point evaluation/order
- sample positions and count
- image writes and output formats
- generated image contents / image quality
- existing pacing and DeferredZero behavior

The rejected paths remain closed: barrier weakening/removal, smaller workgroups, the B10 split path, B5 predicate canonicalization, generic SSA/mem2reg cleanup, reduced precision/formats, speculative pass fusion, and scheduler replacement.

## Most promising IR3-guided questions

### 1. Shared-memory dependency pairs

Map every IR3 shared-memory load/store back to its reduction phase and address expression. Look specifically for a store followed by a same-address reload inside a phase where the value remains available and no barrier/intervening alias requires memory visibility.

A candidate is valid only if the redundant pair is proven in the generated executable and its removal leaves the barrier graph and reduction order unchanged.

### 2. Address/index calculation feeding shared memory

Identify integer address arithmetic immediately feeding shared-memory operations. Only attempt a SPIR-V rewrite when the resulting IR3 demonstrably removes instructions or dependency edges; source/SPIR-V simplification without an executable change is not useful.

### 3. Live ranges crossing reduction barriers

The high SY/SS stall counts plus eight resident waves make register lifetime and barrier-adjacent dependencies worth inspecting. Map values live across each barrier and look for translator-generated temporaries whose lifetime can be shortened without recomputation that changes FP order.

The objective is not generic register-count reduction. It is to remove a specific long dependency chain or spill/reload sequence visible in IR3.

### 4. Spill/reload evidence

Before another Mipmaps or Beta4 lifetime pass, identify explicit scratch/private-memory traffic or compiler spill/reload sequences in the executable. B9 is not evidence against this direction because its intended Beta4 optimization never applied (`phase0-sample-count`, `applied=0`). A corrected lifetime candidate is justified only when the executable points to a concrete target.

## Retention gate for any future shader candidate

A shader candidate should not reach device benchmarking unless all of these are true:

1. The matcher fails closed on every unrecognized shader shape.
2. The intended transformation is proven to apply exactly where expected.
3. IR3/pipeline executable output materially changes in the intended way.
4. The workgroup, barriers, reduction order, FP order, samples, writes, and outputs remain unchanged.
5. A normal device capture shows a timing improvement beyond established run-to-run noise after workload/thermal context is considered.
6. No hitching, artifact, or adaptive transition regression appears in the device capture.

## B12 evidence workflow

B12 is measurement instrumentation, not a shader mutation. The normal evidence build uses:

```sh
LSFGVK_ADAPTIVE_RUNTIME=1 \
LSFGVK_B12_DUAL_STAGE_PROFILE=1 \
./scripts/build/android.sh Release
```

It measures exact Mipmaps dispatch time and exact `p_beta[4]` dispatch time using slot-local query pools and delayed readback after the slot's existing synchronization.

Some Android Vulkan wrappers expose the Turnip device with `timestampValidBits=0`. The clean runtime must continue to honor that report. B12 therefore has an evidence-only fallback that:

1. checks both legacy and `GetPhysicalDeviceQueueFamilyProperties2` reports;
2. records the raw `timestampPeriod` and queue-family diagnostics;
3. if both valid-bit reports remain zero, creates a temporary timestamp query pool and performs one startup command-buffer/queue/readback self-test on the already-selected compute queue;
4. enables the unreported-timestamp path only if creation, submission, fence wait, result retrieval, and an advancing counter all succeed;
5. otherwise fails closed and leaves B12 GPU timing disabled.

Expected diagnostic markers are `b12-timestamp-capability` and `b12-timestamp-fallback`. This mechanism is intentionally absent from the clean runtime.

For a Mipmaps refinement candidate, add pipeline executable/IR capture without enabling the old per-frame heavyweight profiler:

```sh
LSFGVK_ADAPTIVE_RUNTIME=1 \
LSFGVK_B12_DUAL_STAGE_PROFILE=1 \
LSFGVK_B12_MIPMAPS_EXEC_PROFILE=1 \
./scripts/build/android.sh Release
```

This reuses the B6 `VK_KHR_pipeline_executable_properties` capture for `p_mipmaps` only. It is intended for candidate-development builds because the IR dump is verbose, but its runtime cost is concentrated at pipeline creation rather than every frame.

Analyze a capture with:

```sh
python3 scripts/analyze-adreno-mipmaps-evidence.py \
  --candidate candidate.log --json
```

or compare two evidence captures with:

```sh
python3 scripts/analyze-adreno-mipmaps-evidence.py \
  --baseline baseline.log \
  --candidate candidate.log \
  --json
```

The analyzer reconstructs chunked `p_mipmaps` pipeline IR, hashes the executable text, counts NOP/shared-load/shared-store/SS/SY/spill markers, records subgroup/statistic data including resident waves when supplied by Turnip, and computes sample-weighted B12 Mipmaps/Beta4 timing. It flags increased spill markers or reduced resident waves as rejection risks.

The default comparison bands intentionally reflect the historical noise floor:

- fewer than 30 Mipmaps samples: insufficient evidence;
- absolute timing delta below 0.05 ms: noise band;
- at least 0.10 ms lower Mipmaps time plus a changed executable and no risk flags: promising;
- at least 0.10 ms slower: regression signal requiring explanation before retention.

These thresholds are gates for investigation, not a substitute for checking workload, clocks, saturation, temperature, image quality, and frame pacing.

## Current-pass boundary

Do not introduce another Mipmaps shader mutation from aggregate stall counts alone. The next candidate should be authored only after B12 timing plus executable evidence localizes one concrete dependency/address/lifetime target. Keep B11, the Android submit cleanup, adaptive behavior, and DeferredZero unchanged unless a separate measurement demonstrates a specific problem.

This preserves the evidence needed for a genuine Mipmaps breakthrough without risking the stable, hitch-free path for a speculative low-confidence change.
