# B14 Mipmaps Tail Fusion Design

## Status

Approved for implementation on top of retained B13.

The retained native baseline is commit `6d698f8` and GameNative pins it at
`982a0386`. The current on-device Mipmaps observation is approximately
4.00 ms. The B14 retention target is at most 3.20 ms under the same sustained
scene and thermal procedure, with unchanged output quality and no pacing or
stability regression.

## Evidence and bottleneck

The translated `p_mipmaps` shader uses a 32x32 workgroup and five workgroup
barriers. Its reduction phases are:

`1024 -> 256 -> 64 -> 16 -> 4 -> 1`

The final two phases currently do the following:

1. Four lanes reduce the remaining sixteen shared values to four values.
2. Those lanes write the four mip5 texels.
3. Those lanes store the four results back to workgroup memory.
4. All 1024 lanes execute the fifth workgroup barrier.
5. Lane zero reloads the four values, reduces them to one, and writes mip6.

The final reduction tree uses the exact floating-point order
`((C + A) + B) + D`, followed by multiplication by 0.25. No scratch spills
were observed in the baseline executable, so B14 targets synchronization,
shared traffic, and dependency overhead rather than register spilling.

## B14 transformation

Immediately after the fourth barrier, lane zero will:

1. Load the same sixteen workgroup values used by the existing 16-to-4 phase.
2. Produce the four mip5 values in registers using the original grouping and
   floating-point evaluation order.
3. Write the same four mip5 texels at the same coordinates.
4. Reduce those four register values using the original 4-to-1 grouping and
   floating-point evaluation order.
5. Write the same mip6 texel at the same coordinate.

The original four workgroup stores and fifth workgroup barrier are removed.
The replacement remains a single generic shader implementation. It does not
query or branch on GPU vendor, device, driver, compiler, subgroup width, or
backend identity.

## Fail-closed requirements

The transform applies only to the known translated `p_mipmaps` baseline:

- exact byte count, SPIR-V bound, and stable shader hash must match;
- exactly five workgroup barriers must exist;
- the expected final two structured selections and shared/image operations
  must match their complete instruction shape;
- local size must remain 32x32x1;
- unexpected instructions, IDs, types, operands, or control flow reject the
  transform without producing partially rewritten bytecode;
- a second application is idempotent;
- a successful application emits the B14 evidence marker and structural
  before/after counts.

## Semantic contract

B14 must preserve:

- four source samples and all ten image writes;
- all seven mip outputs, formats, and coordinates;
- the 1024-to-16 phases and their first four barriers;
- the exact tail reduction grouping and FP32 operation order;
- descriptor and pipeline interfaces;
- generated, adaptive, and DeferredZero runtime behavior;
- clean-build behavior when the candidate hook is absent.

The intended structural delta is:

- workgroup barriers: 5 to 4;
- final-tail workgroup stores: 4 to 0;
- tail workgroup loads: 15 to 15;
- logical tail image writes: 5 to 5. The static SPIR-V `OpImageWrite` count
  increases from ten to thirteen because the former single mip5 instruction
  executed by four lanes becomes four explicit lane-zero writes.

## Verification and retention

Implementation starts as an evidence-only transform selected through
`LSFGVK_MIPMAPS_CANDIDATE_SCRIPT` and therefore requires B12 evidence mode.
It is retained only if all of the following hold:

1. The contract test first fails without the implementation and then passes.
2. The transformed SPIR-V satisfies the structural and fail-closed checks.
3. The Android capability/contract suite passes.
4. arm64-v8a and x86_64 evidence builds succeed.
5. Final backend executable evidence shows a changed executable, no spill or
   scratch regression, and no material occupancy regression.
6. Repeated Galaxy S20+ testing measures Mipmaps at no more than 3.20 ms, or
   demonstrates an equivalently material aggregate generated-frame gain that
   survives run variance.
7. Fixed 2x/3x/4x, adaptive, DeferredZero, enable/disable, and quick-menu
   suspend/resume remain stable without new tail stalls.
8. Captured output is visually indistinguishable from B13, including fine
   detail, motion edges, disocclusion, shimmer, and temporal stability.
9. A second backend shows no material regression.

If B14 is structurally correct but misses the performance target, it is not
stacked blindly with another mutation. Executable evidence determines whether
to retain it as a free win, revise the 64-to-1 tail, or proceed to the separate
16x16 lane-coarsened front-end candidate.
