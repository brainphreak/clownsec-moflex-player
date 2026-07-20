#!/bin/sh
# Build tag: YMMDD.N -- N restarts at 1 each day and bumps per build (stored in .buildnum).
# Single year digit ("6" for 2026) keeps the tag short enough for the home header.
d=$(date +%y%m%d | cut -c2-)
f="$(dirname "$0")/.buildnum"
pd=""; pn=0
[ -f "$f" ] && read -r pd pn < "$f"
if [ "$pd" = "$d" ]; then n=$((pn + 1)); else n=1; fi
echo "$d $n" > "$f"
echo "$d.$n"
