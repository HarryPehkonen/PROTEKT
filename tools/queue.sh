#!/bin/bash
# queue.sh -- the slice queue: one file per slice, claimed by rename(2).
#
#   queue.sh claim <runner>          take the first slice in file order; prints its path, exit 1 if none
#   queue.sh done <runner> <file>    move a claimed slice to done/            (loud, exit 2, if not claimed)
#   queue.sh renew <runner>          restart the lease clock on <runner>'s claims
#   queue.sh reclaim                 move claims older than the lease back to queue/
#   queue.sh release <file>          HUMAN: move a claimed slice back to queue/ (a bare `mv` does the same)
#   queue.sh status                  what is queued, claimed (by whom, how old), done
#   queue.sh lease                   print the lease in seconds
#
# Layout, under $QUEUE_ROOT (default: ../../protekt-plan relative to this file's repo):
#   queue/NNN-X.slice            waiting; names sort into order
#   claimed/<runner>/NNN-X.slice taken; the file ITSELF records claimed_by and claimed_at
#   done/NNN-X.slice             landed; never looked at again
# A slice file is key=value:  letter=, brief=, msg=  (paths relative to $QUEUE_ROOT).
#
# THE CLAIM IS A rename.  `mv queue/X claimed/<me>/X` is one rename(2) on one filesystem: it either
# moves the file or fails with ENOENT because someone else already did.  A claimer that loses the
# race gets that failure, takes it to mean "that one is gone", and tries the NEXT slice; when it runs
# out it prints nothing and exits 1.  Nobody reads the file to decide -- the move IS the decision.
set -u

# THE LEASE: 4 hours.  run-slice.sh gives the delegate `timeout 10800` (3 h) and then runs build,
# suite, format and tidy, and the runner then runs the full gate (tens of minutes).  A live runner can
# legitimately hold a claim for about 3.5 h without a heartbeat, so the lease must exceed that, and
# 4 h does with margin; any shorter and a slow-but-alive runner gets its slice stolen, any longer and a
# dead one costs more of the night.  The runner also calls `renew` before the gate.
QUEUE_LEASE_SECONDS=${QUEUE_LEASE_SECONDS:-14400}

ROOT=${QUEUE_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)/protekt-plan}
mkdir -p "$ROOT/queue" "$ROOT/claimed" "$ROOT/done" || exit 2
now() { echo "${QUEUE_NOW:-$(date +%s)}"; }

# stamp <file> <runner>: record who and when IN the file, so `cat` explains a stuck claim.
stamp() {
  grep -v -e '^claimed_by=' -e '^claimed_at=' "$1" > "$1.tmp" 2>/dev/null
  printf 'claimed_by=%s\nclaimed_at=%s\n' "$2" "$(now)" >> "$1.tmp"
  mv "$1.tmp" "$1"
}
# claimed_at <file>: the lease clock.  A claim whose runner died between the rename and the stamp has
# no claimed_at, and is treated as claimed at time 0 -- i.e. it is reclaimable at once, which is the
# safe direction (the slice is merely offered again).
claimed_at() { sed -n 's/^claimed_at=//p' "$1" | tail -1 | grep -E '^[0-9]+$' || echo 0; }

cmd=${1:-}; shift || true
case "$cmd" in
  lease) echo "$QUEUE_LEASE_SECONDS" ;;

  reclaim)
    n=0
    for f in "$ROOT"/claimed/*/*.slice; do
      [ -e "$f" ] || continue
      age=$(( $(now) - $(claimed_at "$f") ))
      if [ "$age" -ge "$QUEUE_LEASE_SECONDS" ]; then
        # the same rename: if two reclaimers race, one wins and the other sees ENOENT
        if mv "$f" "$ROOT/queue/$(basename "$f")" 2>/dev/null; then
          echo "reclaimed $(basename "$f") from $(basename "$(dirname "$f")") (claim aged ${age}s >= ${QUEUE_LEASE_SECONDS}s)" >&2
          n=$((n+1))
        fi
      fi
    done
    echo "$n" ;;

  claim)
    me=${1:?usage: queue.sh claim <runner>}
    "$0" reclaim >/dev/null
    mkdir -p "$ROOT/claimed/$me"
    for f in "$ROOT"/queue/*.slice; do          # glob expands sorted: 010 < 020 < 100
      [ -e "$f" ] || continue
      dest="$ROOT/claimed/$me/$(basename "$f")"
      if mv "$f" "$dest" 2>/dev/null; then       # THE rename.  Won.
        stamp "$dest" "$me"
        echo "$dest"; exit 0
      fi                                         # lost: gone from queue/.  Try the next one.
    done
    exit 1 ;;

  renew)
    me=${1:?usage: queue.sh renew <runner>}
    for f in "$ROOT/claimed/$me"/*.slice; do [ -e "$f" ] && stamp "$f" "$me"; done
    exit 0 ;;

  done)
    me=${1:?usage: queue.sh done <runner> <file>}; s=$(basename "${2:?usage: queue.sh done <runner> <file>}")
    if mv "$ROOT/claimed/$me/$s" "$ROOT/done/$s" 2>/dev/null; then echo "done $s"; exit 0; fi
    echo "queue.sh: $s is not claimed by $me (reclaimed from under it, or never taken)" >&2; exit 2 ;;

  release)
    s=$(basename "${1:?usage: queue.sh release <file>}"); moved=0
    for f in "$ROOT"/claimed/*/"$s"; do
      [ -e "$f" ] || continue
      grep -v -e '^claimed_by=' -e '^claimed_at=' "$f" > "$f.tmp"; mv "$f.tmp" "$f"
      mv "$f" "$ROOT/queue/$s" && { echo "released $s"; moved=1; }
    done
    [ "$moved" = 1 ] || { echo "queue.sh: no claim named $s" >&2; exit 2; } ;;

  status)
    echo "queue/ (next first):"; for f in "$ROOT"/queue/*.slice; do [ -e "$f" ] && echo "  $(basename "$f")"; done
    echo "claimed/ (lease ${QUEUE_LEASE_SECONDS}s):"
    for f in "$ROOT"/claimed/*/*.slice; do
      [ -e "$f" ] && echo "  $(basename "$f")  by $(basename "$(dirname "$f")")  aged $(( $(now) - $(claimed_at "$f") ))s"
    done
    echo "done/:"; for f in "$ROOT"/done/*.slice; do [ -e "$f" ] && echo "  $(basename "$f")"; done
    exit 0 ;;

  *) sed -n '2,11p' "$0" >&2; exit 2 ;;
esac
