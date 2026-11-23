#!/bin/bash
# queue_test.sh -- the witnesses for tools/queue.sh, and for the runner's use of it.
# Every test builds its own queue under a mktemp root and a fake clock (QUEUE_NOW), so nothing here
# touches the real protekt-plan/queue and nothing depends on the wall clock.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
Q="$HERE/queue.sh"
RUNNER="$HERE/pipeline.sh"
FAILS=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; FAILS=$((FAILS+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

new_root() { R=$(mktemp -d); mkdir -p "$R/queue" "$R/claimed" "$R/done" "$R/briefs"; export QUEUE_ROOT="$R"; unset QUEUE_NOW; }
slice() { printf 'letter=%s\nbrief=briefs/%s.txt\nmsg=briefs/%s-msg.txt\n' "$2" "$2" "$2" > "$R/queue/$1-$2.slice"; }

echo "== two runners cannot claim one slice"
new_root; slice 010 G4
rm -f /tmp/qt-a /tmp/qt-b
( "$Q" claim runner-a > /tmp/qt-a 2>/dev/null; echo $? > /tmp/qt-a.rc ) &
( "$Q" claim runner-b > /tmp/qt-b 2>/dev/null; echo $? > /tmp/qt-b.rc ) &
wait
wins=0; [ -s /tmp/qt-a ] && wins=$((wins+1)); [ -s /tmp/qt-b ] && wins=$((wins+1))
check "exactly one of two concurrent claimers wins" '[ "$wins" = 1 ]'
check "the loser exits 1 and prints nothing" '[ "$(cat /tmp/qt-a.rc /tmp/qt-b.rc | sort | tr -d "\n")" = "01" ]'
check "the slice is claimed once, queue empty" '[ -z "$(ls $R/queue)" ] && [ "$(ls $R/claimed/*/ | grep -c slice)" = 1 ]'
# many rounds: 8 claimers on 3 slices -> exactly 3 winners, no duplicates
new_root; slice 010 A; slice 020 B; slice 030 C
for i in 1 2 3 4 5 6 7 8; do ( "$Q" claim "r$i" > "$R/out$i" 2>/dev/null ) & done; wait
check "8 claimers on 3 slices: exactly 3 winners" '[ "$(cat $R/out* | grep -c slice)" = 3 ] && [ "$(cat $R/out* | sort -u | grep -c slice)" = 3 ]'

echo "== queue order is file order"
new_root; slice 030 C; slice 010 A; slice 020 B; slice 100 D
got=""; for i in 1 2 3 4; do got="$got$(basename "$("$Q" claim r)" .slice | cut -d- -f2)"; done
check "claims come out 010,020,030,100 regardless of creation order" '[ "$got" = ABCD ]'

echo "== a dead runner's claim is reclaimed after the limit, and NOT before"
new_root; slice 010 G4
export QUEUE_NOW=1000000; "$Q" claim dead >/dev/null
LEASE=$("$Q" lease)
export QUEUE_NOW=$((1000000+LEASE-1)); "$Q" reclaim >/dev/null
check "one second inside the lease: still claimed" '[ -z "$(ls $R/queue)" ]'
export QUEUE_NOW=$((1000000+LEASE)); "$Q" reclaim >/dev/null
check "at the lease limit: back in queue/" '[ -n "$(ls $R/queue)" ] && [ -z "$(find $R/claimed -name "*.slice")" ]'
check "and claimable again by a live runner" '[ -n "$("$Q" claim alive)" ]'
new_root; slice 010 G4; export QUEUE_NOW=1000000; "$Q" claim r >/dev/null
export QUEUE_NOW=$((1000000+LEASE-10)); "$Q" renew r >/dev/null
export QUEUE_NOW=$((1000000+LEASE+5)); "$Q" reclaim >/dev/null
check "a renewed claim is not reclaimed at the original deadline" '[ -z "$(ls $R/queue)" ]'

echo "== a completed slice ends in done/ and is never picked up again"
new_root; slice 010 G4; slice 020 A3; unset QUEUE_NOW
f=$("$Q" claim r); "$Q" done r "$(basename "$f")" >/dev/null
check "done/ holds it" '[ -e "$R/done/010-G4.slice" ]'
check "claimed/ no longer holds it" '[ -z "$(find $R/claimed -name 010-G4.slice)" ]'
n=$(basename "$("$Q" claim r)")
check "the next claim is the NEXT slice, not the done one" '[ "$n" = 020-A3.slice ]'
export QUEUE_NOW=$((9999999999)); "$Q" reclaim >/dev/null; unset QUEUE_NOW
check "reclaim never resurrects done/" '[ -e "$R/done/010-G4.slice" ] && [ ! -e "$R/queue/010-G4.slice" ]'
check "done of an unclaimed slice is loud (exit non-zero)" '! "$Q" done r 999-X.slice >/dev/null 2>&1'

echo "== a human unsticks a slice by hand"
new_root; slice 010 G4; "$Q" claim stuck >/dev/null
"$Q" release 010-G4.slice >/dev/null
check "release moves the claim back to queue/" '[ -e "$R/queue/010-G4.slice" ]'
new_root; slice 010 G4; "$Q" claim stuck >/dev/null
mv "$R"/claimed/stuck/010-G4.slice "$R/queue/"
check "a bare mv also works, and the slice is claimable" '[ -n "$("$Q" claim again)" ]'
new_root; slice 010 G4; "$Q" claim who >/dev/null
check "status names the claimant" '"$Q" status | grep -q "who"'

echo "== a slice with no brief STOPS the runner loudly"
new_root; slice 010 G4     # no brief file, and the runner must not wait 3 hours in a test
out=$(PIPELINE_BRIEF_WAIT=0 PIPELINE_DRYRUN=1 "$RUNNER" 2>&1); rc=$?
check "the runner exits (non-zero stop)" '[ "$rc" != 0 ]'
check "it says it stopped and names the brief" 'echo "$out" | grep -q "brief" && echo "$out" | grep -qi "stopping"'
check "it did not dispatch" '! echo "$out" | grep -q "DISPATCH"'
check "the slice is released back, not left claimed" '[ -e "$R/queue/010-G4.slice" ]'

echo "== the runner walks the queue in order and ends with done/"
new_root; slice 020 B; slice 010 A; : > "$R/briefs/A.txt"; : > "$R/briefs/B.txt"
out=$(PIPELINE_BRIEF_WAIT=0 PIPELINE_DRYRUN=1 "$RUNNER" 2>&1); rc=$?
check "dry run exits 0" '[ "$rc" = 0 ]'
check "dispatch order is A then B" '[ "$(echo "$out" | grep -o "slice [AB] " | tr -d "\n ")" = "sliceAsliceB" ]'
check "both are in done/, queue empty" '[ -e "$R/done/010-A.slice" ] && [ -e "$R/done/020-B.slice" ] && [ -z "$(ls $R/queue)" ]'

echo
[ "$FAILS" = 0 ] && { echo "queue_test: all passed"; exit 0; }
echo "queue_test: $FAILS FAILED"; exit 1
