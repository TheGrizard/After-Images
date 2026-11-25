#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The simulator's checks: the pages, the knobs, REC, saving, the keys, the screen reader. Each script fails the run on
# a FAIL line.
set -e
cd "$(dirname "$0")/.."
out=app/build/host/shots
mkdir -p "$out"
fail=0
# the app's own code compiles with no warning (one read of a value never set drew nothing in the browser's emulator)
./build.sh --host 2> "$out/build.log" || { cat "$out/build.log"; exit 1; }
if grep -E "^[^ ]*app/[^ ]*: warning:" "$out/build.log"; then echo "FAIL: warnings in the app's code"; fail=1; fi
for t in app/tests/*.txt; do
    echo "== $t"
    log="$out/$(basename "$t" .txt).log"
    rc=0; app/build/host/after-images_host "$t" "$out" > "$log" 2>&1 || rc=$?
    if [ "$rc" -gt 1 ]; then echo "FAIL: $t crashed (exit $rc)"; fail=1; fi
    cat "$log"
    if grep -q FAIL "$log"; then fail=1; fi
done
exit $fail
