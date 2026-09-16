# B14 Mipmaps Tail Fusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the final shared-memory Mipmaps reduction rendezvous with an exact register-resident lane-zero tail and target at least a 20% reduction from the current approximately 4.00 ms baseline.

**Architecture:** Keep B13 as the normal runtime baseline and add B14 through the checked Mipmaps evidence-candidate seam. A pure C++ SPIR-V rewriter owns strict baseline recognition and tail replacement; a small Python build transform wires it into translated `p_mipmaps` only. B14 remains evidence-only until executable, device, quality, and stability gates pass.

**Tech Stack:** Python 3 contract tests and build transforms; C++20; SPIR-V 1.x word rewriting; Vulkan compute; Android/Bionic builds; optional IR3 executable analysis.

**Spec:** `docs/plans/2026-09-16-b14-mipmaps-tail-fusion-design.md`

## Global Constraints

- Retained native baseline is `6d698f8`; GameNative baseline pin is `982a0386`.
- Current Mipmaps observation is approximately 4.00 ms; retention target is at most 3.20 ms.
- Preserve all seven mip outputs, ten logical per-workgroup image writes, formats, coordinates, and exact FP32 tail evaluation order. Four explicit mip5 writes raise the static `OpImageWrite` count from ten to thirteen.
- Preserve adaptive, DeferredZero, synchronization, presentation, and pacing behavior.
- No runtime GPU vendor, device, driver, compiler, subgroup-width, or backend branch.
- Clean builds without the candidate hook remain free of B12/B14 instrumentation and markers.
- The rewriter must fail closed and must never return partially rewritten bytecode.

---

### Task 1: Baseline and B14 contract

**Files:**
- Modify: `docs/plans/2026-09-16-final-mipmaps-beta4-stretch-ledger.md`
- Create: `tests/android_candidate_b14_mipmaps_tail_fusion_test.py`
- Create: `tests/fixtures/p_mipmaps_b13.spv`
- Create: `tests/fixtures/README.md`

**Interfaces:**
- Consumes: the retained B13 build path and the captured translated `p_mipmaps` instruction shape.
- Produces: an executable contract for `b14::fuseTail(std::vector<uint8_t>&)` and the candidate build wiring.

- [x] **Step 1: Record B13 as the retained baseline**

Add the accepted B13 commits, controlled B11-to-B13 gains, current approximately
4.00 ms Mipmaps observation, and B14 target of at most 3.20 ms to the final
stretch ledger.

- [x] **Step 2: Write the failing B14 contract test**

The test compiles a small C++ harness against the real B14 rewriter and a
controlled SPIR-V fixture. It must assert the exact structural delta, unchanged
image writes/local size, exact arithmetic-result parity over literal edge and
randomized inputs, idempotence, and rejection after fixture mutation. It must
also apply the Python candidate transform to a temporary translation source
and assert that only `p_mipmaps` enters `b14::fuseTail`.

- [x] **Step 3: Run the focused test and verify RED**

Run: `python3 tests/android_candidate_b14_mipmaps_tail_fusion_test.py`

Expected: FAIL because `scripts/b14_mipmaps_tail_fusion.hpp` and
`scripts/apply-candidate-b14-mipmaps-tail-fusion.py` do not exist.

### Task 2: Fail-closed SPIR-V tail rewriter

**Files:**
- Create: `scripts/b14_mipmaps_tail_fusion.hpp`
- Create: `scripts/apply-candidate-b14-mipmaps-tail-fusion.py`
- Modify: `tests/android_candidate_b14_mipmaps_tail_fusion_test.py`

**Interfaces:**
- Produces: `b14::RewriteReport b14::fuseTail(std::vector<uint8_t>& code)`.
- `RewriteReport` exposes `applied`, `alreadyApplied`, before/after barrier,
  workgroup-load/store, and image-write counts, plus a rejection reason.

- [x] **Step 1: Implement strict SPIR-V parsing and baseline recognition**

Parse all instructions without trusting word counts, validate magic/bound/hash,
identify the five barriers and expected final selections, and collect the
structural counts before any mutation.

- [x] **Step 2: Implement the minimal exact tail replacement**

Allocate new result IDs, add only the required integer constants, generate the
lane-zero 16-to-4 and 4-to-1 block, preserve the original reduction operand
order, splice it after barrier four, update the module bound, and validate the
complete output before swapping it into `code`.

- [x] **Step 3: Implement candidate wiring and evidence marker**

Patch `src/extract/trans.cpp` idempotently to include the rewriter and call it
for `p_mipmaps` after the retained translation transforms. Abort the evidence
build when the known shader does not match; log one marker with the rewrite
report on success.

- [x] **Step 4: Run the focused test and verify GREEN**

Run: `python3 tests/android_candidate_b14_mipmaps_tail_fusion_test.py`

Expected: PASS with the real C++ harness exercising success, idempotence, and
fail-closed rejection.

### Task 3: Repository integration and static evidence

**Files:**
- Modify: `.github/workflows/android-bionic.yml`
- Modify: `tests/android_mipmaps_device_agnostic_evidence_test.py`
- Modify: `docs/plans/2026-09-16-final-mipmaps-beta4-stretch-ledger.md`

**Interfaces:**
- Consumes: `LSFGVK_MIPMAPS_CANDIDATE_SCRIPT` and B12 executable evidence mode.
- Produces: repeatable B13-control and B14-candidate artifact commands with marker audits.

- [x] **Step 1: Add B14 to the policy suite**

Run the focused B14 contract in the Android policy job and assert that the
candidate passes the existing device-agnostic checker.

- [x] **Step 2: Add an opt-in B14 evidence artifact**

Use `LSFGVK_B12_DUAL_STAGE_PROFILE=1`,
`LSFGVK_MIPMAPS_EXEC_PROFILE=1`, and
`LSFGVK_MIPMAPS_CANDIDATE_SCRIPT=scripts/apply-candidate-b14-mipmaps-tail-fusion.py`.
Require B12 timing, executable/IR, and B14-applied markers; forbid B14 from
clean artifacts.

- [x] **Step 3: Run focused and policy tests**

Run the B14 test, Mipmaps device-agnostic test, Mipmaps analyzers, clean-mode
test, and full `tests/android_*_test.py` policy set.

### Task 4: Build and executable verification

**Files:**
- Update: `docs/plans/2026-09-16-final-mipmaps-beta4-stretch-ledger.md`

**Interfaces:**
- Consumes: Android NDK, B12 logs, and optional backend executable properties.
- Produces: clean arm64-v8a/x86_64 artifacts plus paired B13/B14 evidence.

- [ ] **Step 1: Build clean arm64-v8a and x86_64 baselines**

Verify that clean artifacts contain the retained B13 marker and omit B12/B14
profiling or candidate markers.

- [ ] **Step 2: Build the B14 arm64-v8a evidence artifact**

Verify the transform applies once, Vulkan accepts the module, and the artifact
contains the required B12, executable, and B14 markers.

- [ ] **Step 3: Compare final executable evidence**

Use `scripts/analyze-mipmaps-evidence.py` with the optional IR3 adapter. Reject
new spills/scratch, material occupancy loss, unexpected image/shared traffic,
or an unchanged executable.

### Task 5: Device retention decision

**Files:**
- Update: `docs/plans/2026-09-16-final-mipmaps-beta4-stretch-ledger.md`

**Interfaces:**
- Consumes: paired B13/B14 device logs and image-quality captures.
- Produces: retain, revise, or discard decision for B14.

- [ ] **Step 1: Run repeated matched Galaxy S20+ captures**

Exercise fixed 2x/3x/4x and adaptive workloads using the established scenes
and thermal procedure. Measure Mipmaps, aggregate frame-generation overhead,
source/output FPS, GPU wait, and p95/p99 tails.

- [ ] **Step 2: Verify quality and transitions**

Inspect motion edges, fine detail, disocclusion, shimmer, mip stability, and
temporal consistency. Exercise LSFG enable/disable, DeferredZero transitions,
and quick-menu suspend/resume.

- [ ] **Step 3: Validate a second backend and decide**

Confirm no material regression on Xclipse 940 or a desktop Vulkan backend.
Retain B14 only at at most 3.20 ms or with an equivalently material aggregate
win beyond measured variance and no correctness, quality, stability, or tail
regression.
