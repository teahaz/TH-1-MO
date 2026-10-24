#!/bin/sh
# Interrupted captures stay readable: one stopped with SIGINT ends complete, as
# stopped by the user; one killed with SIGKILL is incomplete, and info reads it up to the
# kill. After the kill no line is left driven, and the device starts the next capture
# (the kill left it capturing; the next start resets it). Run from host/ after make.

. test/hw/bench.sh

device_free

file=$HW_DIR/test3-int.thcapture
echo "== SIGINT after 6 s"
HW_BG=1 hw_capture "$file" 0 2>"$HW_DIR/test3-int.log"
pid=$!
sleep 6
kill -INT $pid
wait $pid
code=$?
tail -1 "$HW_DIR/test3-int.log"
[ $code = 0 ] || fail "the capture stopped with SIGINT exited with $code"
[ "$(jget "$file" "j['state']")" = complete ] || fail "the SIGINT file is not complete"
reason=$(jget "$file" "j['end_reason']")
echo "ended: $reason"
lines_released

file=$HW_DIR/test3-kill.thcapture
echo "== SIGKILL after 6 s"
HW_BG=1 hw_capture "$file" 0 2>"$HW_DIR/test3-kill.log"
pid=$!
sleep 6
kill -KILL $pid
wait $pid
lines_released
$BIN info "$file" || fail "info can't read the killed capture"
[ "$(jget "$file" "j['complete']")" = False ] || fail "the killed capture claims to be complete"
n=$(jget "$file" "j['data_channels'][0]['records']")
[ "$n" -gt 0 ] || fail "nothing read back from the killed capture"
echo "$n records read back from the killed capture"

echo "== the device after the kill"
hw_capture "$HW_DIR/test3-after.thcapture" 3 2>"$HW_DIR/test3-after.log" ||
    fail "the capture after the kill exited with $?"
tail -1 "$HW_DIR/test3-after.log"

echo "PASS test 3"
