#!/bin/sh
# Chip-level decoding is right, on the reference captures in $REF_DIR (no device needed):
#  - the SHT3x and INA219 readings decoded from the I2C bus match the values the target
#    prints in its log on the UART, for the same capture (chips_vs_log.py);
#  - every chip reading is understood: none flagged;
#  - the text and JSON output is what was saved next to each capture.
# Skipped when the pendrive is not there. With --update, saves the outputs as the expected
# ones instead of comparing (only after checking them by hand). Run from host/ after make.

. test/hw/bench.sh

update=0
[ "$1" = "--update" ] && update=1

if ! ls "$REF_DIR"/bench-*.thcapture >/dev/null 2>&1; then
    echo "SKIP test 5: no reference captures in $REF_DIR"
    exit 0
fi

tmp=$HW_DIR/test5
mkdir -p "$tmp"
chips="--i2c scl=$BENCH_SCL,sda=$BENCH_SDA --chip $BENCH_SHT3X --chip $BENCH_INA219"
uart="--uart tx=$BENCH_UART,baud=$BENCH_UART_BAUD"

for file in "$REF_DIR"/bench-*.thcapture; do
    name=$(basename "$file" .thcapture)
    ref=$REF_DIR/$name
    echo "== $name"

    $BIN decode "$file" $chips >"$tmp/$name.chips.txt" || fail "decode chips exited with $?"
    $BIN decode "$file" $chips --format jsonl >"$tmp/$name.chips.jsonl" ||
        fail "decode chips jsonl exited with $?"
    $BIN decode "$file" $uart --format raw >"$tmp/$name.log" || fail "decode uart exited with $?"

    grep -q '= SHT3x   read     fetch data' "$tmp/$name.chips.txt" ||
        fail "$name: no SHT3x measurements"
    grep -q '= INA219  read     bus' "$tmp/$name.chips.txt" || fail "$name: no INA219 readings"
    if grep -E '^ += ' "$tmp/$name.chips.txt" | grep -q '\['; then
        grep -E '^ += .*\[' "$tmp/$name.chips.txt" | head -5
        fail "$name: a chip reading is flagged"
    fi
    echo "$(grep -cE '^ += ' "$tmp/$name.chips.txt") chip readings, none flagged"

    python3 -I test/hw/chips_vs_log.py "$tmp/$name.chips.jsonl" "$tmp/$name.log" ||
        fail "$name: the chip readings disagree with the target's log"

    for out in chips.txt chips.jsonl; do
        if [ $update = 1 ]; then
            command cp -f "$tmp/$name.$out" "$ref.$out"
        elif ! cmp -s "$tmp/$name.$out" "$ref.$out"; then
            diff "$ref.$out" "$tmp/$name.$out" | head -20
            fail "$name: $out differs from the expected $ref.$out"
        fi
    done
done

[ $update = 1 ] && echo "expected outputs saved in $REF_DIR"
echo "PASS test 5"
