#!/usr/bin/env bash
#
# Local CI — every gate this repo has, in one script, with no service anywhere.
#
# from AI-DEV-STARTER (plunk-in kit) — if you edit this file, say why in INCIDENTS.md.
#
# No GitHub, no network, no framework: this is what the git hooks in .githooks/ run,
# and you can run it by hand at any time (it is non-destructive — nothing is committed,
# staged, reverted or reformatted for you).
#
#   tools/ci.sh                      # all default stages, in order
#   tools/ci.sh build tests          # just these stages, in the order given
#   tools/ci.sh --list               # what the stages are
#   tools/ci.sh --write-tidy-baseline   # accept the tidy findings you inherited
#   tools/ci.sh --help
#
#   git config core.hooksPath .githooks     # one-time, per clone, enables the hooks
#
# Two tiers, because a C++ full run is minutes and a commit cannot afford minutes. Both lists
# live below as CI_FAST_STAGES and CI_FULL_STAGES, and the hooks NAME a tier rather than
# repeating its stages, so a stage cannot be added to the gate and forgotten by a hook:
#
#   fast  (pre-commit)  format build tests
#   full  (pre-push)    --require-clean tree format kitprobes build tests release version
#                       asan tsan tidy pristine
#
# `format` is in the fast tier because it is the one check that says "the file you are about to
# commit is not the file the formatter would write" — measured at well under a second on a warm
# tree, and the cheap half of the rule the full tier enforces on the way out.
#
# A repo whose spec needs a stage the kit does not ship (a fuzzer, say) appends it to
# CI_FULL_STAGES here. That is the only place it has to be added: the hook and the guard that
# checks these lists both read the variable.
#
# Configuration lives in .ci.env (gitignored, optional); every knob has a default here,
# so the repo works with no config at all. See .ci.env.example.
#
# Exit status: 0 only if every stage that ran passed.
#
# One deliberate reading of "report every failure": a failing stage prints EVERYTHING
# that failed inside it (every unformatted file, every failing test, every finding) and
# then STOPS the run, because these stages are a chain — once the build fails, the
# tests, sanitizers and pristine stages would only repeat the same compiler error with
# more noise. Stages that do not depend on each other (tree, format, version) are
# cheap and run first for that reason. Where a run stops, the summary says so.

set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO_ROOT" || exit 1

# No colour from the tools: this script greps their output (warning:, error:) and ANSI
# escapes defeat the greps. The escapes this script prints itself are for the human.
export NO_COLOR=1

# git exports GIT_INDEX_FILE to a hook when the commit is made with a PATHSPEC
# (`git commit -- <path>`): it names git's TEMPORARY index for that one commit, not this
# repository's index. Every process a hook starts inherits it, and any `git` command the
# gate runs inside ANOTHER repository — cmake's FetchContent update step in a `build`
# stage, a `pip install git+https://…` requirement, a probe that clones into a temp dir, a
# repo's own kitprobes script — then reads THIS repo's index entries against that
# repository's object store and dies on the first blob it does not have:
#     fatal: unable to read 691e2bdafaf312970644391de042d38c2c5972d8
#     CMake Error at .../jsom-populate-gitupdate.cmake:186 (message): Failed to get the status
# — so a pathspec commit fails its OWN gate at `build`, naming a dependency update, on a
# tree that builds fine (measured 2026-09-20 on Computo, cards t_9541aa62 -> t_0a9a0018).
#
# Unset it once, here, rather than `env -u` in front of each command that shells out to
# git: the variable reaches everything the gate starts, so a per-invocation fix would
# cover the instance and leave the class. It is also the safe place, not merely the cheap
# one — measured on a real pathspec commit in a throwaway clone, with the gate's own
# stages run both ways: every input the `tree` and `format` stages read is identical and
# their output is byte-identical, and the two views that do differ (`git diff --cached
# HEAD`, the staged-vs-unstaged letter) are read by no stage. `probes/git-index-file.sh`
# holds every copy to this by running the block below inside a real pathspec commit.
unset GIT_INDEX_FILE

# ---------------------------------------------------------------- defaults + config
CI_JOBS=${CI_JOBS:-$(nproc 2>/dev/null || echo 4)}
CI_BUILD_DIR=${CI_BUILD_DIR:-build}
CI_ASAN_BUILD_DIR=${CI_ASAN_BUILD_DIR:-build-asan}
CI_TSAN_BUILD_DIR=${CI_TSAN_BUILD_DIR:-build-tsan}
CI_LOG_DIR=${CI_LOG_DIR:-.ci-logs}
CI_STRICT_TOOLS=${CI_STRICT_TOOLS:-0}           # 1 = a missing tool fails instead of SKIPping
CI_KEEP_TMP=${CI_KEEP_TMP:-0}                   # 1 = keep the pristine temp dir for inspection
CI_FAST_STAGES=${CI_FAST_STAGES:-"format build tests"}
CI_FULL_STAGES=${CI_FULL_STAGES:-"tree format kitprobes build tests release version asan tsan tidy pristine"}
CI_DEFAULT_STAGES=${CI_DEFAULT_STAGES:-$CI_FULL_STAGES}
CI_TIDY_BASELINE=${CI_TIDY_BASELINE:-.ci/tidy-baseline.txt}
CI_BUILD_TYPE=${CI_BUILD_TYPE:-Debug}
# The SECOND configuration, built and tested by the `release` stage. A gate whose every
# stage builds one build type cannot see the class the other one produces (a `warning:`
# that only exists under -O2/-O3, code that only misbehaves with NDEBUG). Point this at
# whatever the repo's own pipeline builds; Release is the usual answer.
CI_RELEASE_BUILD_DIR=${CI_RELEASE_BUILD_DIR:-build-release}
CI_RELEASE_BUILD_TYPE=${CI_RELEASE_BUILD_TYPE:-Release}
# The source set the format and tidy stages own. Extend for your layout.
CI_SOURCE_GLOBS=${CI_SOURCE_GLOBS:-"'*.cpp' '*.cc' '*.cxx' '*.hpp' '*.hh' '*.h'"}
# Where the SECOND copy of the version number lives. Two forms are both fine:
#   the CMake-generated header  (configure_file(version.hpp.in ...) -> ${CMAKE_BINARY_DIR}/generated/version.hpp)
#   a tracked header            (include/<project>/version.hpp) — point this knob at it
CI_VERSION_HEADER=${CI_VERSION_HEADER:-"$CI_BUILD_DIR/generated/version.hpp"}
# The test runner. ctest is the portable default; override with a single test binary if
# your project does not register tests with add_test().
CI_TEST_CMD=${CI_TEST_CMD:-"ctest --test-dir \$CI_BUILD_DIR --output-on-failure -j \$CI_JOBS"}

if [ -f .ci.env ]; then
    # shellcheck disable=SC1091
    . ./.ci.env
fi

REQUIRE_CLEAN=0
ALLOW_UNTRACKED=0
WRITE_TIDY_BASELINE=0
STAGES_REQUESTED=()

# ---------------------------------------------------------------- plumbing
RESULT_LINES=()
FAILED_STAGE=""
RAN_STAGES=()
RUN_TMP_DIRS=()

usage() {
    # The whole leading comment block: every line from 2 to the first blank one. Derived
    # rather than hard-coded, so adding a line to the header cannot silently cut it off.
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Stages:
  tree        every file committed or ignored; .gitignore audit; the gate's own
              footprint (build dirs, logs, .ci.env) is ignored; --require-clean also
              fails on uncommitted changes to tracked files
  format      clang-format drift — dry run against the repo .clang-format
  kitprobes   the kit fixes this copy claims to carry, held to their contracts: every
              script in tools/kit-probes/ checks one kit fix in THIS gate script by name
              and by behaviour (offline, no kit checkout, no build, <1 s). A missing
              directory SKIPs: it means this copy carries no probe yet, not that it is
              behind. The probes and the rule: docs/KIT-REVISION-CONVENTION.md
  build       cmake configure + build, zero warnings (the stage counts them even where
              -Werror is not wired onto a target)
  tests       the test suite (ctest by default), every failure reported
  release     the SAME suite in a SECOND configuration (CI_RELEASE_BUILD_TYPE, Release
              by default): configure, build, count `warning:` in its own log, run the
              tests. Every other stage builds CI_BUILD_TYPE, so without this stage a
              repo whose own pipeline builds an optimized configuration never checks one
  version     one version number: project(VERSION) in CMakeLists.txt == the header the
              build generates/uses, and the number every binary prints for --version
  asan        separate build dir, ASan+UBSan, same suite
  tsan        separate build dir, ThreadSanitizer, same suite
  tidy        clang-tidy, only NEW findings vs CI_TIDY_BASELINE (a baseline file is
              optional; with none, tidy must be clean). Findings compare line-blind and
              clone-blind — capture the baseline with --write-tidy-baseline, never by
              hand (a raw copy of the log matches nothing: see .ci.env.example)
  pristine    git archive HEAD -> temp dir -> configure, build, test: proves the
              COMMITTED tree is complete (catches files that are uncommitted or ignored)

Options:
  --require-clean     make the tree stage fail when tracked files have uncommitted edits
  --allow-untracked   do not fail when untracked, unignored files exist (deliberate escape)
  --strict-tools      a missing tool (clang-format/clang-tidy) fails instead of skipping
  --write-tidy-baseline
                      accept every finding tidy reports now into CI_TIDY_BASELINE (runs the
                      build and tidy stages first); prints, and IS NOT, a gate pass
  --list              list the stages and exit
  --help              this text
EOF
}

ci_begin() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
ci_pass() { RESULT_LINES+=("pass  $1"); }
ci_skip() {
    RESULT_LINES+=("SKIP  $1 ($2)")
    printf '    SKIP: %s\n' "$2"
}

ci_fail() {
    local name="$1" reason="$2" logfile="${3:-}"
    RESULT_LINES+=("FAIL  $name")
    printf '\n\033[1;31mFAILED: %s — %s\033[0m\n' "$name" "$reason"
    if [ -n "$logfile" ] && [ -f "$logfile" ]; then
        printf '    last output from %s:\n' "$logfile"
        tail -n 25 "$logfile" | sed 's/^/      /'
        printf '    full log: %s\n' "$logfile"
    fi
    FAILED_STAGE="$name"
    summary
    printf '\nGATE FAILED\n' >&2
    exit 1
}

summary() {
    printf '\n--- ci summary ---\n'
    local line
    for line in "${RESULT_LINES[@]}"; do printf '  %s\n' "$line"; done
    if [ -n "$FAILED_STAGE" ]; then
        printf '  stopped at: %s\n' "$FAILED_STAGE"
        # A stage that never ran because an earlier one failed must not look like a
        # stage that passed. Naming it is the whole difference between "everything is
        # fine" and "the tests never ran" — this is why the run stops instead of
        # pretending: once the build fails, the test stage has nothing trustworthy to
        # say, so it is reported as blocked, never as green.
        local s r ran
        for s in "${STAGES_REQUESTED[@]:-}"; do
            [ -n "$s" ] || continue
            ran=0
            for r in "${RAN_STAGES[@]:-}"; do
                [ "$r" = "$s" ] && ran=1
            done
            if [ "$ran" = "0" ] && [ "$s" != "$FAILED_STAGE" ]; then
                printf '  BLOCK %s (did not run: the run stopped at %s)\n' "$s" "$FAILED_STAGE"
            fi
        done
    fi
}

require_tool() {  # require_tool <tool> <stage>; returns 1 when the caller should skip
    local tool="$1" stage="$2"
    if command -v "$tool" >/dev/null 2>&1; then return 0; fi
    if [ "$CI_STRICT_TOOLS" = "1" ]; then
        ci_fail "$stage" "$tool not installed (CI_STRICT_TOOLS=1) — nothing was checked"
    fi
    ci_skip "$stage" "$tool not installed — this gate was NOT exercised on this machine"
    return 1
}

# The file set the format gate owns: same list the CMake `format` target should use.
# --others --exclude-standard includes new files that are not committed yet: while
# working, a new source file is not in `git ls-files`, and a gate that cannot see it
# lets it through unformatted until after it is committed (that incident is in
# INCIDENTS.md).
ci_sources() {
    # shellcheck disable=SC2086
    eval "git ls-files --cached --others --exclude-standard -- $CI_SOURCE_GLOBS" | sort -u
}

# clang-tidy needs a compile database entry per translation unit, so headers are not
# passed here: diagnostics inside headers still surface through the sources that include
# them (that is what .clang-tidy's HeaderFilterRegex is for).
ci_tidy_sources() {
    # shellcheck disable=SC2086
    eval "git ls-files --cached --others --exclude-standard -- $CI_SOURCE_GLOBS" \
        | grep -E '\.(cpp|cc|cxx)$' | sort -u
}

# The comparable form of a clang-tidy finding — applied to BOTH sides of the baseline
# comparison, so the file a repo captures and the log this gate just wrote are the same
# shape:
#
#   <repo>/src/foo.cpp:42:7: warning: ...   ->   src/foo.cpp: warning: ...
#
#   * the repo root is stripped: clang-tidy reports the path it was handed by the compile
#     database, which CMake writes as an absolute path, so a baseline captured in one clone
#     names no finding in a checkout at another path (the nightly clean checkout, a
#     colleague's machine) and every inherited finding reads as new;
#   * :line:column is stripped, so the same finding after an unrelated edit above it is
#     still the same finding. This is line-blind on purpose, and the flip side is worth
#     knowing: a SECOND identical finding in a file that already has one collapses into the
#     first. Fix the baselined finding instead of growing the baseline.
#
# `tools/ci.sh --write-tidy-baseline` captures the baseline through this same function, so
# the documented way to accept findings cannot drift from the way they are compared.
tidy_key() {
    awk -v root="$REPO_ROOT/" '
        { i = index($0, root); if (i) $0 = substr($0, i + length(root)); print }' \
        | sed 's/:[0-9]*:[0-9]*:/:/'
}

run_tests() {  # run_tests <build-dir> <logfile>
    # shellcheck disable=SC2086
    eval "$CI_TEST_CMD" > "$2" 2>&1
}

mkdir -p "$CI_LOG_DIR"

# ---------------------------------------------------------------- stages
stage_tree() {
    ci_begin "tree (every file committed or ignored)"
    # The gate creates these; a .gitignore that does not cover them makes the next run
    # fail the moment it writes a log. Create them first: git check-ignore cannot match
    # a directory pattern against a path that does not exist yet.
    mkdir -p "$CI_BUILD_DIR" "$CI_ASAN_BUILD_DIR" "$CI_TSAN_BUILD_DIR" "$CI_RELEASE_BUILD_DIR" "$CI_LOG_DIR"

    local untracked
    untracked=$(git ls-files --others --exclude-standard)
    if [ -n "$untracked" ]; then
        printf '    not committed and not ignored:\n'
        printf '%s\n' "$untracked" | sed 's/^/      /'
        if [ "$ALLOW_UNTRACKED" = "1" ]; then
            printf '    (not failing: --allow-untracked was passed)\n'
        else
            ci_fail tree "$(printf '%s\n' "$untracked" | wc -l) file(s) are neither committed nor ignored — git add them, or add a .gitignore rule"
        fi
    else
        printf '    no untracked, unignored files\n'
    fi

    local dirty
    dirty=$(git status --porcelain --untracked-files=no)
    if [ -n "$dirty" ]; then
        printf '    uncommitted changes to tracked files:\n'
        printf '%s\n' "$dirty" | sed 's/^/      /'
        if [ "$REQUIRE_CLEAN" = "1" ]; then
            ci_fail tree "uncommitted changes to tracked files (that is the point of the push gate)"
        fi
        printf '    (not failing: --require-clean was not passed)\n'
    else
        printf '    no uncommitted changes to tracked files\n'
    fi

    local tracked_ignored
    tracked_ignored=$(git ls-files -i -c --exclude-standard)
    if [ -n "$tracked_ignored" ]; then
        printf '%s\n' "$tracked_ignored" > "$CI_LOG_DIR/tree.log"
        ci_fail tree "tracked files matched by .gitignore (stale rules) — see $CI_LOG_DIR/tree.log"
    fi

    local path missing=0
    for path in "$CI_BUILD_DIR" "$CI_ASAN_BUILD_DIR" "$CI_TSAN_BUILD_DIR" "$CI_RELEASE_BUILD_DIR" "$CI_LOG_DIR" ".ci.env"; do
        if ! git check-ignore -q "$path" 2>/dev/null; then
            printf '    NOT ignored: %s\n' "$path"
            missing=$((missing + 1))
        fi
    done
    if [ "$missing" -gt 0 ]; then
        ci_fail tree "$missing path(s) that the gate itself creates are not in .gitignore — the next run would fail on its own log files"
    fi
    printf '    gate footprint (build dirs, %s, .ci.env) is ignored\n' "$CI_LOG_DIR"
    ci_pass tree
}

stage_format() {
    ci_begin "format (clang-format --dry-run) — the files this branch touches"
    require_tool clang-format format || return 0
    # Only the files this branch touches: the repo has pre-existing non-compliant files
    # and a whole-tree check buries the signal in noise nobody edited. On a clean
    # checkout (nothing ahead of origin/main) fall back to the last commit.
    local base touched
    base="$(git merge-base HEAD origin/main 2>/dev/null || git rev-parse HEAD)"
    # `git diff HEAD` compares the WORKING TREE against HEAD, and skips over the index: a file whose
    # staged copy differs from its working-tree copy (staged, then formatted on disk) appears in
    # neither that diff nor `ls-files --others`, so nothing below would look at it at all. It is also
    # the file most likely to be wrong -- see the staged-copy check further down. Ask for it.
    touched="$(
        { git diff --name-only --diff-filter=ACMR "$base" HEAD
          git diff --name-only --diff-filter=ACMR HEAD
          git diff --cached --name-only --diff-filter=ACMR
          git ls-files --others --exclude-standard
        } | sort -u
    )"
    if [ -z "$touched" ]; then
        touched="$(git show --name-only --pretty=format: HEAD | sed '/^$/d')"
        printf '    (level with origin/main: checking the last commit instead)\n'
    fi
    local -a sources=()
    while IFS= read -r f; do
        [ -n "$f" ] && [ -f "$f" ] && sources+=("$f")
    done < <(printf '%s\n' "$touched" | grep -E '\.(cpp|cc|cxx|hpp|hh|h)$')
    if [ "${#sources[@]}" -eq 0 ]; then
        printf '    nothing to check\n'
        ci_pass format
        return 0
    fi
    if clang-format --dry-run -Werror "${sources[@]}" > "$CI_LOG_DIR/format.log" 2>&1; then
        # The check above read the WORKING TREE, and a commit records the INDEX. Stage an unformatted
        # file, then format it on disk -- which is what anyone does after this stage rejects a commit --
        # and the check above passes while the commit still records the unformatted text. HEAD then
        # differs from the working tree, and the next `--require-clean` push fails in `tree` with
        # "uncommitted changes to tracked files": a message that never mentions formatting. Measured
        # 2026-10-06 in a throwaway clone (kit fix `format-checks-staged`). So check the staged copy too.
        local -a staged=() index_drift=()
        local f staged_f
        while IFS= read -r staged_f; do
            [ -n "$staged_f" ] && [ -f "$staged_f" ] && [[ "$staged_f" =~ \.(cpp|cc|cxx|hpp|hh|h)$ ]] && staged+=("$staged_f")
        done < <(git diff --cached --name-only --diff-filter=ACMR)
        if [ "${#staged[@]}" -gt 0 ]; then
            for f in "${staged[@]}"; do
                git show ":$f" 2>/dev/null |
                    clang-format --dry-run -Werror --assume-filename="$f" - > /dev/null 2>> "$CI_LOG_DIR/format.log" ||
                    index_drift+=("$f")
            done
            if [ "${#index_drift[@]}" -gt 0 ]; then
                printf '    the STAGED copy is not formatted (that is what the commit would record):\n'
                printf '%s\n' "${index_drift[@]}" | sed 's/^/      /'
                ci_fail format "the staged copy of the file(s) above fails clang-format -- this stage checks the working tree, and a commit records the index (fix: clang-format -i <files> && git add <files>)" "$CI_LOG_DIR/format.log"
                return 1
            fi
        fi
        printf '    %s file(s) conform to .clang-format\n' "${#sources[@]}"
        ci_pass format
        return 0
    fi
    grep -oE '^[^:]+\.(cpp|cc|cxx|hpp|hh|h)' "$CI_LOG_DIR/format.log" | sort -u | sed 's/^/      /'
    ci_fail format "clang-format drift in the files listed above (fix: clang-format -i <those files>)" "$CI_LOG_DIR/format.log"
}

# ---------------------------------------------------------------- kit probes
# A fix that must propagate ships a probe (docs/KIT-REVISION-CONVENTION.md). Each script
# in tools/kit-probes/ holds this gate to ONE kit fix's contract — name and behaviour, not
# bytes — and exits non-zero when the fix is absent. The directory IS the list of fixes
# this copy claims to carry, so absence fails HERE, in under a second, on the machine that
# would otherwise push the lag: no kit checkout, no network, no build.
#
# Why not a hash or a diff against the kit: the copies of this file are forks (a repo's
# adapted stages, its own defaults, 60-586 differing lines), and diff SIZE measures
# divergence, not lateness — a one-fix-behind copy is missing 27 kit lines while a
# verified current record is missing 125. See the convention for the measurement.
#
# It is name- and contract-level: a semantic regression INSIDE a function that is still
# present is not caught. That needs a real build and a real run, which is what the port
# did by hand; the probe is the cheap net, not the whole net.
stage_kitprobes() {
    ci_begin "kit probes (the fixes this copy claims to carry)"
    local self probe name failed=0
    self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    if [ ! -d "$REPO_ROOT/tools/kit-probes" ]; then
        ci_skip kitprobes "no tools/kit-probes/ — this copy carries no kit probe yet"
        return 0
    fi
    for probe in "$REPO_ROOT"/tools/kit-probes/*.sh; do
        [ -f "$probe" ] || continue
        name="$(basename "$probe")"
        if bash "$probe" "$self" "$REPO_ROOT"; then
            printf '    ok   %s\n' "$name"
        else
            printf '    FAIL %s — this gate is missing that kit fix\n' "$name"
            failed=1
        fi
    done
    if [ "$failed" = "1" ]; then
        ci_fail kitprobes "a probe failed: this copy is behind a kit fix — port it from the kit (tools/kit-probes/ names which)"
    fi
    printf '    every probe in tools/kit-probes/ verified against this gate\n'
    ci_pass kitprobes
}

stage_build() {
    ci_begin "build (-Werror, zero warnings)"
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_BUILD_DIR" -DCMAKE_BUILD_TYPE="$CI_BUILD_TYPE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/configure.log" 2>&1 \
        || ci_fail build "cmake configure failed" "$CI_LOG_DIR/configure.log"
    cmake --build "$CI_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/build.log" 2>&1 \
        || ci_fail build "build failed (-Werror is on: a warning is a build failure)" "$CI_LOG_DIR/build.log"
    # -Werror only covers the targets it is wired onto, so a target that never got the
    # flag would build with warnings and pass. Count them here too.
    local warns
    warns=$(grep -c 'warning:' "$CI_LOG_DIR/build.log" || true)
    if [ "${warns:-0}" -gt 0 ]; then
        grep 'warning:' "$CI_LOG_DIR/build.log" | head -5 | sed 's/^/      /'
        ci_fail build "$warns compiler warning(s) in a target -Werror does not cover" "$CI_LOG_DIR/build.log"
    fi
    printf '    built with no warnings\n'
    ci_pass build
}

stage_tests() {
    ci_begin "tests"
    if [ ! -f "$CI_BUILD_DIR/CMakeCache.txt" ]; then
        ci_fail tests "no build configured in $CI_BUILD_DIR — run the build stage first"
    fi
    if ! run_tests "$CI_BUILD_DIR" "$CI_LOG_DIR/tests.log"; then
        grep -E "FAILED|Failed|\*\*\*Failed|assert" "$CI_LOG_DIR/tests.log" | head -30 | sed 's/^/      /'
        ci_fail tests "test failures (all of them are above; full output in the log)" "$CI_LOG_DIR/tests.log"
    fi
    grep -E "tests passed|100% tests passed" "$CI_LOG_DIR/tests.log" | tail -1 | sed 's/^/      /'
    ci_pass tests
}

# The optimized configuration, in its own build dir. Every other stage builds
# CI_BUILD_TYPE=Debug, while the repo's own pipeline (a workflow that passes no build type,
# a ./build.sh, a consumer that defaults -O3) can build something else entirely - so the
# configuration that really runs can be the one no stage ever configures. Measured cost of
# the class (Computo, 2026-09-20): a red Pages deploy for twelve days and a ./build.sh that
# had been broken on the developer's own box the whole time, behind a GREEN local gate.
# Both the warning rule and the test command match the Debug stages on purpose: the point is
# the SAME code under optimizations, not a differently-graded run. Full tier only: a cold
# optimized build is 88-146 s, then ~5 s on a one-source push and ~0 s unchanged, because
# the build dir is reused.
stage_release() {
    ci_begin "release ($CI_RELEASE_BUILD_TYPE: the configuration an optimized build uses)"
    # No -DCMAKE_EXPORT_COMPILE_COMMANDS here: the compile database is the `build` stage's
    # (tidy reads $CI_BUILD_DIR), and a second one would only be a decoy.
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_RELEASE_BUILD_DIR" -DCMAKE_BUILD_TYPE="$CI_RELEASE_BUILD_TYPE" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/release-configure.log" 2>&1 \
        || ci_fail release "cmake configure failed" "$CI_LOG_DIR/release-configure.log"
    cmake --build "$CI_RELEASE_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/release-build.log" 2>&1 \
        || ci_fail release "optimized build failed (-Werror is on: a warning is a build failure)" "$CI_LOG_DIR/release-build.log"
    # The same rule the `build` stage applies, to this log instead: -Werror only covers the
    # targets it is wired onto, and an optimization-dependent diagnostic is a warning before
    # it is an error. This is the check that reports the class the Debug stages cannot see.
    local warns
    warns=$(grep -c 'warning:' "$CI_LOG_DIR/release-build.log" || true)
    if [ "${warns:-0}" -gt 0 ]; then
        grep 'warning:' "$CI_LOG_DIR/release-build.log" | head -5 | sed 's/^/      /'
        ci_fail release "$warns compiler warning(s) in an optimized build" "$CI_LOG_DIR/release-build.log"
    fi
    printf '    built with no warnings at %s\n' "$CI_RELEASE_BUILD_TYPE"
    local saved="$CI_TEST_CMD"
    # shellcheck disable=SC2086
    CI_TEST_CMD="$(printf '%s' "$saved" | sed "s|\$CI_BUILD_DIR|$CI_RELEASE_BUILD_DIR|g")"
    if ! run_tests "$CI_RELEASE_BUILD_DIR" "$CI_LOG_DIR/release-tests.log"; then
        grep -E "FAILED|Failed|\*\*\*Failed|assert" "$CI_LOG_DIR/release-tests.log" | head -30 | sed 's/^/      /'
        ci_fail release "test failures in an optimized build (all of them are above; full output in the log)" "$CI_LOG_DIR/release-tests.log"
    fi
    CI_TEST_CMD="$saved"
    grep -E "tests passed|100% tests passed" "$CI_LOG_DIR/release-tests.log" | tail -1 | sed 's/^/      /'
    ci_pass release
}

stage_version() {
    ci_begin "version (two copies of one number must agree)"
    local cmake_version
    cmake_version=$(sed -n 's/^project *( *[A-Za-z0-9_.-]* *VERSION *\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
    if [ -z "$cmake_version" ]; then
        ci_fail version "could not read a VERSION from CMakeLists.txt — project(<name> VERSION <x.y.z>) is what this stage parses"
    fi

    local header="$CI_VERSION_HEADER"
    if [ ! -f "$header" ]; then
        # Help, not guesswork: say which files could hold the second copy.
        local candidates
        candidates=$(find "$CI_BUILD_DIR" -type f \( -name '*version*.hpp' -o -name '*version*.h' \) 2>/dev/null | sort | head -5)
        if [ -n "$candidates" ]; then
            printf '    %s not found. Version-named headers in %s:\n' "$header" "$CI_BUILD_DIR"
            printf '%s\n' "$candidates" | sed 's/^/      /'
        fi
        ci_fail version "the header holding the second copy of the number is not at $CI_VERSION_HEADER — point CI_VERSION_HEADER at it (a tracked header works too)"
    fi
    local header_version
    # Any of:  #define APP_VERSION "1.2.3"   |   static constexpr char VERSION[] = "1.2.3";
    header_version=$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+[^"]*"' "$header" | head -1 | tr -d '"')
    if [ -z "$header_version" ]; then
        ci_fail version "$header holds no quoted x.y.z version string"
    fi
    if [ "$cmake_version" != "$header_version" ]; then
        printf '    CMakeLists.txt says %s, %s says %s\n' "$cmake_version" "$header" "$header_version"
        ci_fail version "the two copies of the version number disagree (bump both from the same edit)"
    fi
    printf '    %s == %s == %s\n' "CMakeLists.txt" "$header" "$cmake_version"

    # And, optionally, the binaries have to report it: a version nobody can ask for is
    # not a version. Off by default because every project names its executables
    # differently — set CI_VERSION_BINARIES="$CI_BUILD_DIR/myapp*" in .ci.env when the
    # naming is stable (see .ci.env.example).
    if [ -n "${CI_VERSION_BINARIES:-}" ]; then
        local binary reported checked=0 wrong=0
        # eval, exactly like CI_TEST_CMD: the documented value is "$CI_BUILD_DIR/myapp",
        # and a plain word-split would carry that through as an unexpanded literal, match
        # no file, and then print "0 binary/binaries report <version>" -- a PASS. That is
        # the fails-open shape this script exists to prevent, so the miss is also a FAIL
        # below.
        local binary_list
        eval "binary_list=\"$CI_VERSION_BINARIES\""
        for binary in $binary_list; do
            [ -f "$binary" ] && [ -x "$binary" ] || continue
            reported=$("$binary" --version 2>&1 | head -1)
            case "$reported" in
                *"$cmake_version"*) checked=$((checked + 1)) ;;
                *) wrong=$((wrong + 1)); printf '      %s --version printed: %s\n' "$(basename "$binary")" "$reported" ;;
            esac
        done
        if [ "$wrong" -gt 0 ]; then
            ci_fail version "$wrong binary/binaries do not report a --version containing $cmake_version"
        fi
        if [ "$checked" -eq 0 ]; then
            ci_fail version "CI_VERSION_BINARIES=\"$CI_VERSION_BINARIES\" matched no executable — the binaries were NOT checked"
        fi
        printf '    %s binary/binaries report %s\n' "$checked" "$cmake_version"
    else
        printf '    (CI_VERSION_BINARIES unset: the binaries were not asked for --version)\n'
    fi
    ci_pass version
}

stage_asan() {
    ci_begin "asan (ASan+UBSan, separate build dir)"
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_ASAN_BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/asan-configure.log" 2>&1 \
        || ci_fail asan "cmake configure failed" "$CI_LOG_DIR/asan-configure.log"
    cmake --build "$CI_ASAN_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/asan-build.log" 2>&1 \
        || ci_fail asan "sanitizer build failed" "$CI_LOG_DIR/asan-build.log"
    # CI_ASAN_TEST_CMD: optional, for tests that cannot run under sanitizers at all
    # (RSS-based leak heuristics, wall-clock benchmarks). Defaults to CI_TEST_CMD, so
    # without it the sanitizer run is the same command in a different build dir.
    local saved="$CI_TEST_CMD"
    # shellcheck disable=SC2086
    CI_TEST_CMD="$(printf '%s' "${CI_ASAN_TEST_CMD:-$CI_TEST_CMD}" | sed "s|\$CI_BUILD_DIR|$CI_ASAN_BUILD_DIR|g")"
    if ! UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 \
        run_tests "$CI_ASAN_BUILD_DIR" "$CI_LOG_DIR/asan-tests.log"; then
        grep -E "ERROR: AddressSanitizer|runtime error|FAILED" "$CI_LOG_DIR/asan-tests.log" | head -20 | sed 's/^/      /'
        ci_fail asan "ASan/UBSan reported something" "$CI_LOG_DIR/asan-tests.log"
    fi
    CI_TEST_CMD="$saved"
    printf '    clean under ASan+UBSan\n'
    ci_pass asan
}

stage_tsan() {
    ci_begin "tsan (ThreadSanitizer, separate build dir)"
    # A thread sanitizer on a single-threaded suite still catches static/shared state
    # touched from more than one thread — the bug this gate exists for.
    # shellcheck disable=SC2086
    cmake -S . -B "$CI_TSAN_BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/tsan-configure.log" 2>&1 \
        || ci_fail tsan "cmake configure failed" "$CI_LOG_DIR/tsan-configure.log"
    cmake --build "$CI_TSAN_BUILD_DIR" -j "$CI_JOBS" > "$CI_LOG_DIR/tsan-build.log" 2>&1 \
        || ci_fail tsan "ThreadSanitizer build failed" "$CI_LOG_DIR/tsan-build.log"
    local saved="$CI_TEST_CMD"
    CI_TEST_CMD="$(printf '%s' "$saved" | sed "s|\$CI_BUILD_DIR|$CI_TSAN_BUILD_DIR|g")"
    if ! TSAN_OPTIONS=halt_on_error=1 run_tests "$CI_TSAN_BUILD_DIR" "$CI_LOG_DIR/tsan.log"; then
        grep -E "WARNING: ThreadSanitizer|data race|FAILED" "$CI_LOG_DIR/tsan.log" | head -20 | sed 's/^/      /'
        ci_fail tsan "ThreadSanitizer reported a data race (or a wrong answer)" "$CI_LOG_DIR/tsan.log"
    fi
    CI_TEST_CMD="$saved"
    printf '    no data races reported\n'
    ci_pass tsan
}

stage_tidy() {
    ci_begin "tidy (clang-tidy, only NEW findings)"
    require_tool clang-tidy tidy || return 0
    if [ ! -f "$CI_BUILD_DIR/compile_commands.json" ]; then
        ci_fail tidy "no $CI_BUILD_DIR/compile_commands.json — configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (the build stage does) before tidy can say anything"
    fi
    local -a sources=()
    mapfile -t sources < <(ci_tidy_sources)
    if [ "${#sources[@]}" -eq 0 ]; then
        ci_fail tidy "no sources matched $CI_SOURCE_GLOBS — tidy would analyse nothing and still pass"
    fi

    # A compile database that exists is not a compile database that covers this repo:
    # CMake can write one that holds a dependency's translation units and none of ours.
    # clang-tidy then analyses nothing, finds no header, and still prints a result.
    local uncovered=0 src
    for src in "${sources[@]}"; do
        if ! grep -qF "\"$REPO_ROOT/$src\"" "$CI_BUILD_DIR/compile_commands.json"; then
            printf '    not in the compile database: %s\n' "$src"
            uncovered=$((uncovered + 1))
        fi
    done
    if [ "$uncovered" -gt 0 ]; then
        ci_fail tidy "$uncovered of ${#sources[@]} sources are missing from $CI_BUILD_DIR/compile_commands.json — the result would be a lie"
    fi
    printf '    compile database covers all %s sources\n' "${#sources[@]}"

    printf '%s\n' "${sources[@]}" \
        | xargs -P "$CI_JOBS" -n 1 clang-tidy -p "$CI_BUILD_DIR" > "$CI_LOG_DIR/tidy.log" 2>&1
    local findings
    findings=$(grep -cE 'warning:|error:' "$CI_LOG_DIR/tidy.log" || true)
    if [ "${findings:-0}" -gt 0 ] && [ -f "$CI_TIDY_BASELINE" ]; then
        local new_findings
        # Both sides go through tidy_key, and both sides are de-duplicated: one key per
        # distinct finding. A baseline captured by `--write-tidy-baseline` is already in
        # that form; anything else in the file is normalised here rather than trusted.
        new_findings=$(comm -13 \
            <(tidy_key < "$CI_TIDY_BASELINE" | sort -u) \
            <(grep -E "warning:|error:" "$CI_LOG_DIR/tidy.log" | tidy_key | sort -u) | wc -l)
        if [ "$new_findings" -gt 0 ]; then
            ci_fail tidy "$new_findings new finding(s) vs $CI_TIDY_BASELINE" "$CI_LOG_DIR/tidy.log"
        fi
        printf '    no new findings vs %s (%s total)\n' "$CI_TIDY_BASELINE" "$findings"
    elif [ "${findings:-0}" -gt 0 ]; then
        grep -E 'warning:|error:' "$CI_LOG_DIR/tidy.log" | sed 's/^/      /' | head -20
        ci_fail tidy "$findings finding(s) and no baseline file — accept them in one step with 'tools/ci.sh --write-tidy-baseline', or fix them; see .ci.env.example" "$CI_LOG_DIR/tidy.log"
    else
        printf '    %s files clean\n' "${#sources[@]}"
    fi
    ci_pass tidy
}

# Capture the accepted-findings baseline. Not a stage: the list is compared against every
# finding in the log, not against the outcome of the run, and the run is EXPECTED to fail
# while there is no baseline yet. It runs the real stages as a child so there is exactly one
# definition of what a finding is (tidy_key) and of where tidy's log comes from.
write_tidy_baseline() {
    printf '\n\033[1m==> write the tidy baseline\033[0m (%s)\n' "$CI_TIDY_BASELINE"
    printf '    running the real build + tidy stages (build first: tidy refuses to run without\n'
    printf '    %s/compile_commands.json; treat the tidy failure below as expected)\n\n' "$CI_BUILD_DIR"
    bash "$0" build tidy > "$CI_LOG_DIR/write-baseline.log" 2>&1
    if [ ! -f "$CI_LOG_DIR/tidy.log" ]; then
        printf 'no %s was written, so there is nothing to capture — the run stopped before the tidy stage:\n' "$CI_LOG_DIR/tidy.log" >&2
        tail -n 20 "$CI_LOG_DIR/write-baseline.log" | sed 's/^/      /' >&2
        printf '    full log: %s\n' "$CI_LOG_DIR/write-baseline.log" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$CI_TIDY_BASELINE")"
    grep -E 'warning:|error:' "$CI_LOG_DIR/tidy.log" | tidy_key | sort -u > "$CI_TIDY_BASELINE"
    local accepted
    accepted=$(grep -c . "$CI_TIDY_BASELINE" || true)
    printf '    %s finding(s) accepted into %s:\n' "${accepted:-0}" "$CI_TIDY_BASELINE"
    head -20 "$CI_TIDY_BASELINE" | sed 's/^/      /'
    if [ "${accepted:-0}" -eq 0 ]; then
        printf '\nNothing to accept: the tidy stage is clean, so delete %s and keep the stage strict.\n' "$CI_TIDY_BASELINE"
        exit 0
    fi
    printf '\nThis is NOT a gate pass: from now on the tidy stage tolerates exactly these findings\n'
    printf 'and fails on anything else. Commit the file — it is this repo'"'"'s accepted-findings list,\n'
    printf 'and the tree stage fails on a file that is neither committed nor ignored:\n'
    printf '    git add %s && git commit -m "ci: accept the inherited clang-tidy findings"\n' "$CI_TIDY_BASELINE"
}

stage_pristine() {
    ci_begin "pristine (does the COMMITTED tree build on its own?)"
    local tmp
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/ci-pristine-XXXXXX")
    RUN_TMP_DIRS+=("$tmp")
    git archive HEAD | tar -x -C "$tmp" || ci_fail pristine "git archive HEAD failed"
    printf '    HEAD checked out: %s files\n' "$(find "$tmp" -type f | wc -l)"

    # shellcheck disable=SC2086
    cmake -S "$tmp" -B "$tmp/build" -DCMAKE_BUILD_TYPE="$CI_BUILD_TYPE" \
        ${CI_CMAKE_FLAGS:-} > "$CI_LOG_DIR/pristine-configure.log" 2>&1 \
        || ci_fail pristine "a fresh checkout of HEAD does not even configure (a needed file is not committed)" "$CI_LOG_DIR/pristine-configure.log"
    cmake --build "$tmp/build" -j "$CI_JOBS" > "$CI_LOG_DIR/pristine-build.log" 2>&1 \
        || ci_fail pristine "a fresh checkout of HEAD does not build" "$CI_LOG_DIR/pristine-build.log"
    local saved="$CI_TEST_CMD"
    CI_TEST_CMD="$(printf '%s' "$saved" | sed "s|\$CI_BUILD_DIR|$tmp/build|g")"
    if ! run_tests "$tmp/build" "$CI_LOG_DIR/pristine-tests.log"; then
        ci_fail pristine "tests fail on a fresh checkout of HEAD" "$CI_LOG_DIR/pristine-tests.log"
    fi
    CI_TEST_CMD="$saved"
    tail -n 2 "$CI_LOG_DIR/pristine-tests.log" | sed 's/^/      /'
    if [ "$CI_KEEP_TMP" = "1" ]; then
        printf '    kept: %s\n' "$tmp"
    else
        rm -rf "$tmp"
    fi
    ci_pass pristine
}

cleanup() {
    local dir
    if [ "$CI_KEEP_TMP" != "1" ]; then
        for dir in "${RUN_TMP_DIRS[@]:-}"; do
            [ -n "$dir" ] && [ -d "$dir" ] && rm -rf "$dir"
        done
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- dispatch
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --list)
            printf 'default stages: %s\n' "$CI_DEFAULT_STAGES"
            # Derived from the defined functions, so this cannot drift from the stages
            # the script actually implements.
            printf 'stages:'
            for fn in $(declare -F | awk '{print $3}' | grep '^stage_' | sed 's/^stage_//' | sort); do
                printf ' %s' "$fn"
            done
            printf '\n'
            exit 0 ;;
        fast) STAGES_REQUESTED+=($CI_FAST_STAGES) ;;
        full) STAGES_REQUESTED+=($CI_FULL_STAGES) ;;
        --require-clean) REQUIRE_CLEAN=1 ;;
        --allow-untracked) ALLOW_UNTRACKED=1 ;;
        --strict-tools) CI_STRICT_TOOLS=1 ;;
        --write-tidy-baseline) WRITE_TIDY_BASELINE=1 ;;
        -*) printf 'unknown option: %s (try --help)\n' "$1" >&2; exit 2 ;;
        *) STAGES_REQUESTED+=("$1") ;;
    esac
    shift
done

# Accepting inherited findings is its own mode, not a stage: it produces no verdict about
# the tree, only a file (and it exits before the summary so it can never print
# "GATE PASSED" about a run whose tidy stage failed on purpose).
if [ "$WRITE_TIDY_BASELINE" = "1" ]; then
    write_tidy_baseline
    exit 0
fi

if [ ${#STAGES_REQUESTED[@]} -eq 0 ]; then
    # shellcheck disable=SC2206
    STAGES_REQUESTED=($CI_DEFAULT_STAGES)
fi

printf '\033[1mlocal CI\033[0m — %s stage(s): %s\n' "${#STAGES_REQUESTED[@]}" "${STAGES_REQUESTED[*]}"
START=$(date +%s)
for stage in "${STAGES_REQUESTED[@]}"; do
    if ! declare -F "stage_$stage" > /dev/null; then
        printf 'unknown stage: %s (try --list)\n' "$stage" >&2
        exit 2
    fi
    # A stage that returns non-zero without reporting a verdict is not a pass, and a stage that
    # dies from a shell error cannot report anything at all — so neither is left to the summary.
    if ! "stage_$stage"; then
        FAILED_STAGE="$stage"
        summary
        printf 'FAILED: %s exited non-zero without reporting a verdict\n' "$stage" >&2
        printf '\nGATE FAILED\n' >&2
        exit 1
    fi
    RAN_STAGES+=("$stage")
done
ELAPSED=$(( $(date +%s) - START ))

# The verdict comes from what RAN, not from what was requested. A shell error can unwind out of
# the loop above without either guard seeing it — measured 2026-09-20 on Computo's fork: `set -u`
# plus `local -a sources` (declared, never filled) made "${#sources[@]}" an unbound-variable
# error, which aborted stage_format and the dispatch loop together, and the run then printed
# "all 10 stage(s) passed ... GATE PASSED" after executing one stage of ten (INCIDENTS.md). This
# comparison is the backstop for that whole class: if any requested stage did not run, the run
# fails.
if [ "${#RAN_STAGES[@]}" -ne "${#STAGES_REQUESTED[@]}" ]; then
    summary
    printf 'FAILED: %s of %s stage(s) did not run — the run ended early\n' \
        "$(( ${#STAGES_REQUESTED[@]} - ${#RAN_STAGES[@]} ))" "${#STAGES_REQUESTED[@]}" >&2
    printf '  ran: %s\n' "${RAN_STAGES[*]:-none}" >&2
    printf '\nGATE FAILED\n' >&2
    exit 1
fi

summary
printf '\nall %s stage(s) passed in %ss\nGATE PASSED\n' "${#STAGES_REQUESTED[@]}" "$ELAPSED"
