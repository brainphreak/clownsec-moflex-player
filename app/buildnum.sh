#!/bin/sh
# Build tag: YYMMDD.N -- N restarts at 1 each day and bumps per build (stored in .buildnum).
d=$(date +%y%m%d)
f="$(dirname "$0")/.buildnum"
pd=""; pn=0
[ -f "$f" ] && read -r pd pn < "$f"
if [ "$pd" = "$d" ]; then n=$((pn + 1)); else n=1; fi
echo "$d $n" > "$f"
echo "$d.$n"
