"""
The SHT3x and INA219 readings bus-replay decodes from the I2C bus against
the values the bench target prints in its log on the UART, for the same capture.

usage: chips_vs_log.py CHIPS.jsonl LOG.txt

CHIPS.jsonl is `decode --i2c .. --chip sht3x@.. --chip ina219@..,shunt=..,cal=.. -f jsonl`,
LOG.txt the UART's `-f raw` bytes. The target reads both chips once a second and then logs
  Temperature 21.18 C, humidity 63.85 %RH
  UPS 4.044 V, 0.006 A, 0.040 W, battery 87 %
so the decoded cycles and the logged lines are two lists of the same readings. The log is
buffered and the capture starts and ends anywhere, so the lists are lined up by the offset
that matches best; every reading in the overlap must then match the log to its last
printed digit (one digit off allowed for rounding).
"""

import json
import re
import sys


def readings(path):
    """Per cycle: the SHT3x reading, then the INA219's last bus, current and power."""
    cycles = []
    for line in open(path):
        item = json.loads(line)
        for child in item.get("children", []):
            f = child["fields"]
            if child["kind"] == "sht3x" and "temp" in f:
                cycles.append({"temp": f["temp"], "rh": f["rh"]})
            elif child["kind"] == "ina219" and cycles:
                for key in ("bus", "current", "power"):
                    if key in f:
                        cycles[-1][key] = f[key]
    return [c for c in cycles if len(c) == 5]


def logged(path):
    text = open(path, "rb").read().decode("ascii", "replace")
    temps = re.findall(r"Temperature (-?[\d.]+) C, humidity ([\d.]+) %RH", text)
    ups = re.findall(r"UPS ([\d.]+) V, (-?[\d.]+) A, (-?[\d.]+) W", text)
    lines = []
    for (t, rh), (v, a, w) in zip(temps, ups):
        lines.append({"temp": t, "rh": rh, "bus": v, "current": a, "power": w})
    return lines


def close(value, printed):
    """value, rounded as the log prints it, equals printed or is one digit off."""
    digits = len(printed.split(".")[1]) if "." in printed else 0
    step = 10 ** -digits
    return abs(round(value, digits) - float(printed)) <= step * 1.01


def main():
    dec = readings(sys.argv[1])
    log = logged(sys.argv[2])
    if not dec or not log:
        print(f"FAIL: {len(dec)} decoded cycles, {len(log)} logged ones")
        return 1

    best = None
    for off in range(-len(log) + 1, len(dec)):
        pairs = [(dec[i], log[i - off]) for i in range(len(dec)) if 0 <= i - off < len(log)]
        good = sum(all(close(d[k], g[k]) for k in g) for d, g in pairs)
        if best is None or good > best[1]:
            best = (off, good, pairs)

    off, good, pairs = best
    exact = sum(all(round(d[k], len(g[k].split(".")[1])) == float(g[k]) for k in g)
                for d, g in pairs)
    print(f"{len(dec)} decoded cycles, {len(log)} logged; {len(pairs)} lined up, "
          f"{good} match, {exact} of them to the last digit")
    if good != len(pairs) or len(pairs) < min(len(dec), len(log)) - 2:
        for d, g in pairs:
            if not all(close(d[k], g[k]) for k in g):
                print("  decoded", {k: round(d[k], 4) for k in d}, "logged", g)
        print("FAIL: the decoded readings do not match the log")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
