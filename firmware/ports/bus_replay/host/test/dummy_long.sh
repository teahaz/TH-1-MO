#!/bin/sh
# A long dummy capture on the pendrive, then the recording checks at full size: info from the END
# chunk, the index rebuilt and compared with the one written live, and a capture killed
# with SIGKILL read back. Run from host/ after make.
#
# usage: test/dummy_long.sh [SECONDS]    (default 60)

set -e

dir=/media/pi/PENDRIVE/host-m2
secs=${1:-60}
bin=build/bus-replay

mkdir -p "$dir"
rm -f "$dir"/long.thcapture* "$dir"/killed.thcapture*

echo "== capture, $secs s"
if ! $bin capture --dummy --out "$dir/long.thcapture" --duration "$secs" 2>"$dir/long.log"; then
    tail -3 "$dir/long.log"
    exit 1
fi
tail -1 "$dir/long.log"

echo "== info"
$bin info "$dir/long.thcapture"

echo "== index rebuild"
mv "$dir/long.thcapture.index" "$dir/long.index.live"
$bin info "$dir/long.thcapture" | grep '^index'
if cmp -s "$dir/long.index.live" "$dir/long.thcapture.index"; then
    echo "the rebuilt index is the one written live"
else
    echo "FAIL: the rebuilt index differs from the one written live"
    exit 1
fi
rm "$dir/long.index.live"

echo "== killed with SIGKILL after $((secs / 4 + 3)) s"
$bin capture --dummy --out "$dir/killed.thcapture" --duration 0 2>/dev/null &
pid=$!
sleep $((secs / 4 + 3))
kill -9 "$pid"
wait "$pid" || true
$bin info "$dir/killed.thcapture"
