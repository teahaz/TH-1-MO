# The bench: what is wired to which device channel. The only
# place the hardware tests take channel numbers from; when the device's channels change,
# change them here. Sourced by the other scripts, with the helpers they share.

# Channels, as the device numbers them.
BENCH_SCL=0         # I2C SCL: INA219 (0x43) and SHT3x (0x44)
BENCH_SDA=1         # I2C SDA
BENCH_IDLE=2        # wired, held high, no edges
BENCH_UART=3        # the target's log, UART 115200 8N1
BENCH_UART_BAUD=115200
BENCH_CHANNELS="$BENCH_SCL,$BENCH_SDA,$BENCH_IDLE,$BENCH_UART"
BENCH_ACTIVE="$BENCH_SCL $BENCH_SDA $BENCH_UART"

# The chips on the I2C bus, as --chip bindings. The UPS module's INA219 has a 10 mohm shunt,
# and its driver sets calibration 4096 at boot, before any reference capture.
BENCH_SHT3X="sht3x@0x44"
BENCH_INA219="ina219@0x43,shunt=0.01,cal=4096"

BENCH_RATE=500000
BENCH_C=1000

# Capture files go to the pendrive, never the SD card.
HW_DIR=/media/pi/PENDRIVE/host-m3
REF_DIR=/media/pi/PENDRIVE/thcapture-ref

BIN=${BIN:-build/bus-replay}


# A capture of the bench's channels: hw_capture FILE SECONDS [more capture arguments].
# With HW_BG=1 it runs in the background, and $! is the capture's own pid.
hw_capture() {
    file=$1
    secs=$2
    shift 2
    rm -f "$file" "$file.index"
    if [ "${HW_BG:-0}" = 1 ]; then
        $BIN capture --channels "$BENCH_CHANNELS" --rate "$BENCH_RATE" --c "$BENCH_C" \
            --duration "$secs" --out "$file" "$@" &
    else
        $BIN capture --channels "$BENCH_CHANNELS" --rate "$BENCH_RATE" --c "$BENCH_C" \
            --duration "$secs" --out "$file" "$@"
    fi
}

# A value from `info --json`: jget FILE EXPR [info arguments], EXPR in Python on j.
jget() {
    file=$1
    expr=$2
    shift 2
    $BIN info "$file" --json "$@" | python3 -I -c "import json, sys; j = json.load(sys.stdin); print($expr)"
}

# Records of one channel, from `info --json`.
channel_records() {
    jget "$1" "next((c['records'] for c in j['channels'] if c['channel'] == $2), 0)"
}

fail() {
    echo "FAIL: $*"
    exit 1
}

# No line of the SPI control binding or reset may be left driven.
lines_released() {
    for line in 7 8 16 19 24 25; do
        if ! gpioinfo -c gpiochip0 "$line" 2>/dev/null | grep -q 'input'; then
            fail "GPIO$line is not a released input"
        fi
        if gpioinfo -c gpiochip0 "$line" 2>/dev/null | grep -q 'consumer'; then
            fail "GPIO$line is still requested"
        fi
    done
}

# The device must not be in use by another bus-replay: the old host's UI or link, or a
# capture. Matched by argv[0], since the old UI's link runs as /proc/self/exe.
device_free() {
    if pgrep -f '^([^ ]*/)?bus-replay( |$)' >/dev/null; then
        fail "another bus-replay holds the device: $(pgrep -af '^([^ ]*/)?bus-replay( |$)')"
    fi
}

mkdir -p "$HW_DIR"
