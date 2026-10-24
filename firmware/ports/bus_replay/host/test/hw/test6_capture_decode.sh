#!/bin/sh
# capture --decode on the bench: the items decoded live during a 20 s capture
# are the items decode gives from the file recorded alongside, and a capture without
# --out leaves nothing behind. Run from host/ after make.

. test/hw/bench.sh

dir=/media/pi/PENDRIVE/host-m5b
decoders="--i2c scl=$BENCH_SCL,sda=$BENCH_SDA --chip $BENCH_SHT3X --chip $BENCH_INA219 \
--uart tx=$BENCH_UART,baud=$BENCH_UART_BAUD"
file=$dir/live.thcapture

mkdir -p "$dir"
device_free

echo "== capture with --decode, 20 s, recorded"
hw_capture "$file" 20 --decode $decoders -f jsonl >"$dir/live.jsonl" 2>"$dir/live.log"
code=$?
tail -1 "$dir/live.log"
[ $code = 0 ] || fail "capture exited with $code"
lines_released
$BIN decode "$file" $decoders -f jsonl >"$dir/file.jsonl" || fail "decode of the file failed"
n=$(wc -l <"$dir/live.jsonl")
[ "$n" -gt 0 ] || fail "no items decoded live"
cmp -s "$dir/live.jsonl" "$dir/file.jsonl" || fail "live items differ from the file's"
echo "$n items, live and from the file the same"
$BIN capture --channels "$BENCH_CHANNELS" --duration 3 --decode $decoders \
    >"$dir/live.txt" 2>/dev/null
grep -m 1 'SHT3x' "$dir/live.txt" || fail "no SHT3x reading in the text output"

echo "== capture with --decode, 10 s, nothing kept"
before=$(find "$dir" | sort)
bin=$(realpath "$BIN")
(cd "$dir" && "$bin" capture --channels "$BENCH_CHANNELS" --duration 10 \
    --decode $decoders >/dev/null 2>/dev/null) || fail "capture without --out failed"
after=$(find "$dir" | sort)
[ "$before" = "$after" ] || fail "a capture without --out wrote files"
lines_released

echo "PASS test 6"
