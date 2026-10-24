#!/bin/sh
# The file holds what was captured: the counts taken live (the status line at
# the end, and the END chunk written from them) are the counts a parse of the file
# gives. Run from host/ after make.

. test/hw/bench.sh

device_free

file=$HW_DIR/test2.thcapture
echo "== capture, 20 s"
hw_capture "$file" 20 2>"$HW_DIR/test2.log" || fail "the capture exited with $?"
last=$(tail -1 "$HW_DIR/test2.log")
echo "$last"

live=$(echo "$last" | sed -n 's/.* records \([0-9]*\) .*/\1/p')
trailer=$(jget "$file" "j['data_channels'][0]['records']")
[ "$live" = "$trailer" ] || fail "live $live records, the END chunk says $trailer"

$BIN info "$file" --json >"$HW_DIR/test2.trailer.json"
$BIN info "$file" --json --parse >"$HW_DIR/test2.parsed.json"
if ! cmp -s "$HW_DIR/test2.trailer.json" "$HW_DIR/test2.parsed.json"; then
    diff "$HW_DIR/test2.trailer.json" "$HW_DIR/test2.parsed.json"
    fail "the END chunk and a parse of the file disagree"
fi
echo "$live records live, in the END chunk and in a parse of the file"

echo "PASS test 2"
