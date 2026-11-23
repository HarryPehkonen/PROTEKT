#!/bin/bash
# pipeline.sh -- the slice runner.  One slice, one full gate, one push; stops loudly on a missing brief.
# v23: the queue is a DIRECTORY (tools/queue.sh), claimed by rename -- no more `sed -n "${idx}p"` of a
# file a human may be editing.  A slice is taken by moving it, finished by moving it to done/, and a
# runner that dies leaves a claim that the lease returns to queue/.
#
# Test seams (tools/queue_test.sh):  PIPELINE_BRIEF_WAIT=<polls of 180s, default 60>,
#                                    PIPELINE_DRYRUN=1 (claim and complete, dispatch nothing).
cd "$(dirname "$0")/.." || exit 1
Q=./tools/queue.sh
P=${QUEUE_ROOT:-$(cd .. && pwd)/protekt-plan}
export QUEUE_ROOT=$P
ME="runner-$$-$(date +%s)"
DRY=${PIPELINE_DRYRUN:-0}
MAXWAIT=${PIPELINE_BRIEF_WAIT:-60}
slicefield() { sed -n "s/^$2=//p" "$1" | head -1; }

echo "### pipeline v23 $(date +%H:%M:%S) HEAD $(git log --oneline -1) -- runner $ME"
if [ "$DRY" != 1 ]; then
  ahead=$(git rev-list --count origin/main..HEAD)
  busy=$(pgrep -x claude | wc -l)
  if [ "$ahead" -gt 0 ] && [ "$busy" -eq 0 ] && [ "$(git status --porcelain | wc -l)" -eq 0 ]; then
    echo "  ORPHANS: $ahead unpushed commit(s), no delegate running -- gating and pushing them first"
    ./tools/ci.sh --require-clean full > /tmp/gate-orphan.log 2>&1
    if ! grep -q 'GATE PASSED' /tmp/gate-orphan.log; then
      echo "  ORPHAN GATE FAILED -- stopping so it can be seen."; grep -e 'FAILED:' -e 'stopped at' /tmp/gate-orphan.log | tail -5; exit 1
    fi
    git push --no-verify origin main > /tmp/push-orphan.log 2>&1; echo "  PUSH EXIT: $?"
  fi
fi

while : ; do
  claim=$($Q claim "$ME") || { echo "  queue exhausted"; break; }
  s=$(basename "$claim")
  letter=$(slicefield "$claim" letter); brief=$(slicefield "$claim" brief); msg=$(slicefield "$claim" msg)
  waited=0
  while [ ! -f "$P/$brief" ]; do
    if [ "$waited" -ge "$MAXWAIT" ]; then
      echo "  $letter: brief $brief never appeared -- stopping"
      $Q release "$s" >/dev/null; exit 1      # hand the slice back so a human sees it queued, not stuck
    fi
    echo "  $letter: waiting for $brief ($(date +%H:%M))"; sleep 180; waited=$((waited+1))
  done
  echo "### slice $letter starts $(date +%H:%M:%S) ($s)"
  if [ "$DRY" = 1 ]; then $Q done "$ME" "$s" || exit 1; continue; fi
  while [ -n "$(pgrep -x claude)" ]; do sleep 30; $Q renew "$ME"; done
  bash "$P/run-slice.sh" "$letter" "$P/$brief" "$P/$msg" "$letter" \
    || { echo "  $letter DID NOT COMMIT -- stopping (claim kept in claimed/$ME/; 'queue.sh release $s' to retry)"; exit 1; }
  echo "  $letter committed: $(git log --oneline -1)"
  ./tools/ci.sh tidy 2>&1 | tail -2
  $Q renew "$ME"
  echo "--- GATE for $letter ($(date +%H:%M:%S)) ---"
  ./tools/ci.sh --require-clean full > /tmp/gate-$letter.log 2>&1
  if ! grep -q 'GATE PASSED' /tmp/gate-$letter.log; then
    echo "  GATE FAILED for $letter -- NOT PUSHING.  Holding for a fix (claim kept)."
    grep -e 'FAILED:' -e 'BLOCK' -e 'stopped at' /tmp/gate-$letter.log | tail -8
    exit 1
  fi
  grep -e '^  pass ' -e '^  SKIP' -e 'passed in' /tmp/gate-$letter.log | tail -12
  echo "=== PUSH $letter ($(date +%H:%M:%S)) ==="
  git push --no-verify origin main > /tmp/push-$letter.log 2>&1; echo "PUSH EXIT: $?"
  Lh=$(git rev-parse HEAD); Rh=$(git ls-remote origin main | cut -f1)
  echo "  local  $Lh"; echo "  remote $Rh"
  if [ "$Lh" = "$Rh" ]; then $Q done "$ME" "$s"; echo "  PUSH VERIFIED: pull and play"
  else echo "  NOT CONFIRMED -- claim kept"; exit 1; fi
done
echo "### PIPELINE DONE $(date +%H:%M:%S)"
