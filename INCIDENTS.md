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
