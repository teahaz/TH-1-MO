#!/bin/sh
# Capture works: a capture of the bench's channels has no CRC errors, gap errors
# or unexplained mismatches (the exit code says so), records on every active line and
# none on the idle one. Stopping leaves the device ready: a second capture right after
# works too. Run from host/ after make.

. test/hw/bench.sh

device_free

for run in 1 2; do
    file=$HW_DIR/test1-$run.thcapture
    secs=$([ $run = 1 ] && echo 20 || echo 5)

    echo "== capture $run, $secs s"
    hw_capture "$file" "$secs" 2>"$HW_DIR/test1-$run.log"
    code=$?
    tail -2 "$HW_DIR/test1-$run.log"
    [ $code = 0 ] || fail "capture $run exited with $code"

    [ "$(jget "$file" "j['state']")" = complete ] || fail "capture $run: the file is not complete"
    for ch in $BENCH_ACTIVE; do
        n=$(channel_records "$file" "$ch")
        [ "$n" -gt 0 ] || fail "capture $run: no records on channel $ch"
        echo "channel $ch: $n records"
    done
    n=$(channel_records "$file" "$BENCH_IDLE")
    [ "$n" = 0 ] || fail "capture $run: $n records on the idle channel $BENCH_IDLE"
    lines_released
done

echo "PASS test 1"
