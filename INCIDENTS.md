# Incidents

Edits to `tools/ci.sh` (the AI-DEV-STARTER gate copy), with the reason, as that file's own header
asks.

## 2026-10-17 — `asan` stage passes LSAN_OPTIONS

The stage ran with `ASAN_OPTIONS=detect_leaks=1` and no suppression file, so a leak inside libdrm
reached through `SDL_Init(SDL_INIT_VIDEO)` (DRM device enumeration) failed the gate with all 134
tests passing. The stage now points `LSAN_OPTIONS` at `.ci/lsan.supp` when that file exists. One
entry, `leak:drmModeObjectGetProperties`, scoped to the function rather than the library so a leak
anywhere else still fails. Nothing else about the stage changed.

## 2026-10-20 — `format` skips `third_party/`

The vendored stb headers (`third_party/stb/*.h`) matched the format stage's source filter, so it
reported them as drifted from `.clang-format` -- code that is not ours to reformat. The stage's file
list now drops paths under `third_party/`. (Tidy needs no change: it only takes `.cpp` files, and
`.clang-tidy`'s header filter does not cover `third_party/`.) Nothing else about the stages changed.

## 2026-10-25 — `tidy` analyses only what the change can reach

`tidy` was the single largest stage: 618s of the full tier's 1606s. It has no incremental mode, so
it analysed all 119 translation units on every push, including a push that touched no C++ at all.
The stage now narrows its list first (`ci_tidy_scope`, `CI_TIDY_SCOPE=1`): the changed `.cpp` files,
plus every TU whose depfile in `build/` names a changed header. Nothing is guessed -- the depfiles
are the compiler's own record of what included what. Three outcomes are stated explicitly: a
smaller list, a SKIP when the change reaches nothing tidy reads, and the whole list when the set
cannot be computed. A set smaller than the truth would be a lie.

Measured on this 4-core machine: whole corpus 618s; a docs-only or `ci.sh`-only change SKIPs in
0.099s; a one-`.cpp` change 1.1s; a change to `include/protekt/world/world.hpp` (96 depfiles, 89 in
the universe) ~910s under contention and FAILED with 25 distinct findings, as it must. The
`--write-tidy-baseline` writer forces `CI_TIDY_SCOPE=0`, because a baseline captured from a partial
run would report the rest as new.

A defect was found while testing this and fixed in the same change: "computed and the change reaches
nothing" and "could not compute" both returned non-zero, so a clean tree was analysed in full
instead of skipped. They are now separate (0 with empty output vs 2), because exit status is the
only channel between them. Verified: a clean tree SKIPs in 0.099s instead of sweeping all 119.
