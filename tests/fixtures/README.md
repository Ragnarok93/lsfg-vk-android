# B14 Mipmaps fixture

`p_mipmaps_b13.spv` is the translated `p_mipmaps` module captured from the
retained B13 lineage before any B14 mutation. Its SHA-256 is
`68c68ffd7308d0cc742aa3e9ecbd00f44c92893c23df5cd62e1318cdb75b9046`.

The fixture is test evidence only. It is never packaged into a runtime
artifact. The B14 contract uses it to exercise the same fail-closed C++ SPIR-V
rewriter that the runtime candidate invokes.
