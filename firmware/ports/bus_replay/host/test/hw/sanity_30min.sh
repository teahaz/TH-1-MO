#!/bin/sh
# The sanity check: a 30 minute capture of the bench to the pendrive with no overflow
# events and no errors. Run by hand, from host/ after make.

. test/hw/bench.sh

device_free

file=$HW_DIR/sanity-30min.thcapture
echo "== capture, 30 min (status in $HW_DIR/sanity-30min.log)"
hw_capture "$file" 1800 2>"$HW_DIR/sanity-30min.log"
code=$?
tail -2 "$HW_DIR/sanity-30min.log"
[ $code = 0 ] || fail "the capture exited with $code"

$BIN info "$file"
[ "$(jget "$file" "j['overflow_bytes']")" = 0 ] || fail "data was dropped (overflow)"
if grep -q 'no data for\|dropped\|control:' "$HW_DIR/sanity-30min.log"; then
    grep 'no data for\|dropped\|control:' "$HW_DIR/sanity-30min.log"
    fail "events during the capture"
fi
lines_released

echo "PASS sanity check"
