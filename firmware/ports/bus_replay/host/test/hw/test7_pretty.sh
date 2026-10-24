#!/bin/sh
# --pretty on the reference captures in $REF_DIR (no device needed):
#  - with both chips bound, every line is a chip reading, a UART burst's text, or the text
#    going on below; there is a line for every chip reading of the full text, and none for
#    a transaction;
#  - with only the SHT3x bound, the INA219's transactions stay, as header lines without
#    bytes, one for each transaction to its address in the full text.
# Skipped when the pendrive is not there. Run from host/ after make.

. test/hw/bench.sh

if ! ls "$REF_DIR"/bench-*.thcapture >/dev/null 2>&1; then
    echo "SKIP test 7: no reference captures in $REF_DIR"
    exit 0
fi

tmp=$HW_DIR/test7
mkdir -p "$tmp"
i2c="--i2c scl=$BENCH_SCL,sda=$BENCH_SDA"
uart="--uart tx=$BENCH_UART,baud=$BENCH_UART_BAUD"
ina_addr=${BENCH_INA219#*@}
ina_addr=${ina_addr%%,*}

for file in "$REF_DIR"/bench-*.thcapture; do
    name=$(basename "$file" .thcapture)
    echo "== $name"

    $BIN decode "$file" $i2c $uart --chip "$BENCH_SHT3X" --chip "$BENCH_INA219" \
        >"$tmp/$name.full.txt" || fail "decode exited with $?"
    $BIN decode "$file" $i2c $uart --chip "$BENCH_SHT3X" --chip "$BENCH_INA219" --pretty \
        >"$tmp/$name.pretty.txt" || fail "decode --pretty exited with $?"

    other=$(grep -vcE '^ +[0-9]+\.[0-9]{6}  (SHT3x|INA219) |^ +[0-9]+\.[0-9]{6}  uart +> "|^ +"' \
        "$tmp/$name.pretty.txt")
    if [ "$other" != 0 ]; then
        grep -vE '^ +[0-9]+\.[0-9]{6}  (SHT3x|INA219) |^ +[0-9]+\.[0-9]{6}  uart +> "|^ +"' \
            "$tmp/$name.pretty.txt" | head -5
        fail "$name: $other lines are not a chip reading or UART text"
    fi
    readings=$(grep -cE '^ += ' "$tmp/$name.full.txt")
    lines=$(grep -cE '^ +[0-9]+\.[0-9]{6}  (SHT3x|INA219) ' "$tmp/$name.pretty.txt")
    [ "$readings" = "$lines" ] ||
        fail "$name: $readings chip readings in the full text, $lines in the pretty one"
    bursts=$(grep -cE '^ +[0-9]+\.[0-9]{6}  uart ' "$tmp/$name.full.txt")
    texts=$(grep -cE '^ +[0-9]+\.[0-9]{6}  uart ' "$tmp/$name.pretty.txt")
    [ "$bursts" = "$texts" ] || fail "$name: $bursts UART bursts, $texts in the pretty text"
    echo "$lines chip readings, $texts UART bursts, nothing else"

    $BIN decode "$file" $i2c --chip "$BENCH_SHT3X" >"$tmp/$name.sht.txt" ||
        fail "decode exited with $?"
    $BIN decode "$file" $i2c --chip "$BENCH_SHT3X" --pretty >"$tmp/$name.sht-pretty.txt" ||
        fail "decode --pretty exited with $?"
    want=$(grep -cE "^ +[0-9]+\.[0-9]{6}  i2c +$ina_addr " "$tmp/$name.sht.txt")
    got=$(grep -cE "^ +[0-9]+\.[0-9]{6}  i2c +$ina_addr " "$tmp/$name.sht-pretty.txt")
    [ "$want" -gt 0 ] && [ "$want" = "$got" ] ||
        fail "$name: $want transactions to $ina_addr in the full text, $got in the pretty one"
    if grep -qE '^ +[<>] ' "$tmp/$name.sht-pretty.txt"; then
        fail "$name: the pretty text shows bytes"
    fi
    echo "$got transactions to $ina_addr with no chip bound, as headers"
done

echo "PASS test 7"
