#!/bin/sh
# The UI's session through the link, on the bench: bus-replay link on the device, and a
# session through the remote backend (the Lua binding, in CEL's scripted mode) that captures
# the bench's channels for 10 s with an I2C group (and the SHT3x on it) and a packets group.
# Checks that records arrive on every active channel with no CRC errors, that I2C decodes
# transactions with SHT3x readings, and that packets come. The link records the capture.
# Needs a build with CEL. Run from host/ after make.

. test/hw/bench.sh

dir=$HW_DIR
[ -d "$dir" ] && [ -w "$dir" ] || dir=$HOME/scratchpad/host-link
mkdir -p "$dir"
sock=$dir/link.sock
file=$dir/link.thcapture
bin=$(realpath "$BIN")
cel=$(realpath test/hw/test8_link_session.cel)

device_free
rm -f "$file" "$file.index" "$sock"

echo "== link on the device, recording to $file"
"$bin" link --socket "$sock" --record "$file" 2>"$dir/link.log" &
link=$!
i=0
while [ ! -S "$sock" ] && [ $i -lt 100 ]; do
    sleep 0.1
    i=$((i + 1))
done
[ -S "$sock" ] || { kill $link; fail "the link did not start: $(cat "$dir/link.log")"; }

echo "== session through the link, 10 s"
# CEL's scripted mode writes debug.log where it runs: not in host/.
(cd "$dir" && echo "poll" | LINK_SOCK="$sock" SECONDS_RUN=10 BENCH_SCL=$BENCH_SCL \
    BENCH_SDA=$BENCH_SDA BENCH_CHANNELS=$BENCH_CHANNELS BENCH_RATE=$BENCH_RATE \
    BENCH_C=$BENCH_C BENCH_SHT3X=$BENCH_SHT3X "$bin" ui --script "$cel") >"$dir/session.out" 2>&1
code=$?
kill $link
wait $link
lines_released
sed 's/\x1b\[[0-9;]*m//g; s/ *$//' "$dir/session.out" | grep -E '^[a-z0-9_]+ ' >"$dir/session.txt"
cat "$dir/session.txt"
[ $code = 0 ] || fail "the UI exited with $code"

get() {
    awk -v k="$1" '$1 == k { print $2 }' "$dir/session.txt"
}

[ "$(get open)" = ok ] || fail "open: $(get open)"
[ "$(get start)" = ok ] || fail "start: $(get start)"
[ "$(get crc)" = 0 ] || fail "CRC errors: $(get crc)"
[ "$(get gaps)" = 0 ] || fail "gap errors: $(get gaps)"
for ch in $BENCH_ACTIVE; do
    n=$(get "ch$ch")
    [ "${n:-0}" -gt 100 ] || fail "only ${n:-0} records on ch$ch"
done
[ "$(get "ch$BENCH_IDLE")" = 0 ] || fail "edges on the idle ch$BENCH_IDLE"
[ "$(get i2c)" -gt 5 ] || fail "only $(get i2c) I2C transactions"
[ "$(get i2c_at_sht3x)" -gt 0 ] ||
    fail "no I2C traffic at $BENCH_SHT3X (addresses seen: $(get i2c_addresses)): bench.sh out of date?"
[ "$(get sht3x)" -gt 0 ] || fail "no SHT3x readings at $BENCH_SHT3X"
[ "$(get packets)" -gt 5 ] || fail "only $(get packets) packets"
for d in bus packets; do
    [ "$(get "decoder_$d")" = ok ] || fail "decoder $d: $(get "decoder_$d")"
done

echo "== the link's recording"
"$bin" info "$file" | grep -E '^(state|duration|ended|loss)'
records=$(channel_records "$file" "$BENCH_SCL")
[ "${records:-0}" -gt 100 ] || fail "the recording has ${records:-0} records on ch$BENCH_SCL"

echo "PASS test 8"
