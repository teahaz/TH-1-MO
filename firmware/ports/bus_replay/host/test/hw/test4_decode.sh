#!/bin/sh
# Decoding is right, on the reference captures in $REF_DIR (no device needed):
#  - I2C on the bench's SCL and SDA shows the INA219 (0x43) and SHT3x (0x44), every
#    transaction acknowledged, and matches an independent decode (i2c_reference.py);
#  - UART on the bench's log line gives the target's log text with no framing errors;
#  - the text and JSON output is what was saved next to each capture.
# Skipped when the pendrive is not there. With --update, saves the outputs as the expected
# ones instead of comparing (only after checking them by hand). Run from host/ after make.

. test/hw/bench.sh

update=0
[ "$1" = "--update" ] && update=1

if ! ls "$REF_DIR"/bench-*.thcapture >/dev/null 2>&1; then
    echo "SKIP test 4: no reference captures in $REF_DIR"
    exit 0
fi

tmp=$HW_DIR/test4
mkdir -p "$tmp"
i2c="--i2c scl=$BENCH_SCL,sda=$BENCH_SDA"
uart="--uart tx=$BENCH_UART,baud=$BENCH_UART_BAUD"

for file in "$REF_DIR"/bench-*.thcapture; do
    name=$(basename "$file" .thcapture)
    ref=$REF_DIR/$name
    echo "== $name"

    $BIN decode "$file" $i2c >"$tmp/$name.i2c.txt" || fail "decode i2c exited with $?"
    $BIN decode "$file" $uart >"$tmp/$name.uart.txt" || fail "decode uart exited with $?"
    $BIN decode "$file" $i2c $uart --format jsonl >"$tmp/$name.jsonl" ||
        fail "decode jsonl exited with $?"

    grep -q '  0x43  ' "$tmp/$name.i2c.txt" || fail "$name: no INA219 (0x43) transactions"
    grep -q '  0x44  ' "$tmp/$name.i2c.txt" || fail "$name: no SHT3x (0x44) transactions"
    if grep -E '^ *[0-9]+\.[0-9]+ ' "$tmp/$name.i2c.txt" | grep -qv ' ACK$'; then
        fail "$name: a transaction not acknowledged, or flagged"
    fi
    echo "$(grep -c ' ACK$' "$tmp/$name.i2c.txt") I2C transactions, all ACK"

    $BIN export "$file" --csv >"$tmp/$name.csv" || fail "export exited with $?"
    $BIN decode "$file" $i2c --format jsonl >"$tmp/$name.i2c.jsonl"
    python3 -I test/hw/i2c_reference.py "$tmp/$name.csv" "$tmp/$name.i2c.jsonl" \
        "$BENCH_SCL" "$BENCH_SDA" || fail "$name: the independent I2C decode disagrees"

    grep -q 'framing errors' "$tmp/$name.uart.txt" && fail "$name: UART framing errors"
    grep -q 'BUS_REPLAY_TARGET: Temperature' "$tmp/$name.uart.txt" ||
        fail "$name: no readable log text on the UART"
    echo "$(grep -c 'BUS_REPLAY_TARGET' "$tmp/$name.uart.txt") log lines, no framing errors"

    for out in i2c.txt uart.txt jsonl; do
        if [ $update = 1 ]; then
            command cp -f "$tmp/$name.$out" "$ref.$out"
        elif ! cmp -s "$tmp/$name.$out" "$ref.$out"; then
            diff "$ref.$out" "$tmp/$name.$out" | head -20
            fail "$name: $out differs from the expected $ref.$out"
        fi
    done
done

[ $update = 1 ] && echo "expected outputs saved in $REF_DIR"
echo "PASS test 4"
