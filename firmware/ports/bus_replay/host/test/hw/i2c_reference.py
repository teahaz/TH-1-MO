"""Checks bus-replay's I2C decode against an independent decode of the same raw records.

usage: i2c_reference.py RECORDS.csv DECODE.jsonl SCL SDA

RECORDS.csv is `bus-replay export FILE --csv`, DECODE.jsonl `bus-replay decode FILE
--i2c scl=SCL,sda=SDA --format jsonl`. The reference decoder here is separate code with the
same rules: edges of one tick taken as SCL falls, SDA changes, SCL rises; bits read as SCL
rises; nine bits a byte, the ninth the ACK. Every segment (address byte, then data, each
with its ACK) must match. Exit 0 if they all do.
"""
import csv, json, sys

path, jpath, scl_ch, sda_ch = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

rows = list(csv.DictReader(open(path)))
order = lambda r: (int(r["tick"]), 0 if r["channel"] == scl_ch and r["level"] == "0"
                   else 2 if r["channel"] == scl_ch else 1)
scl = sda = 1
bits, seg, ref = [], None, []
for row in sorted(rows, key=order):
    ch, lvl = row["channel"], int(row["level"])
    if ch == sda_ch:
        if scl and lvl == 0 and sda == 1:
            if seg is not None:
                ref.append(seg)
            seg, bits = [], []
        elif scl and lvl == 1 and sda == 0 and seg is not None:
            ref.append(seg)
            seg = None
        sda = lvl
    elif ch == scl_ch:
        if lvl == 1 and scl == 0 and seg is not None:
            bits.append(sda)
            if len(bits) == 9:
                seg.append((int("".join(map(str, bits[:8])), 2), bits[8] == 0))
                bits = []
        scl = lvl
ref = [s for s in ref if s]

ours = []
for line in open(jpath):
    t = json.loads(line)
    words = t.get("words", [])
    # Split the words by segment span.
    for s in t.get("children", []):
        ws = [w for w in words if s["start"] <= w["start"] <= s["end"]]
        if ws:
            ours.append([(w["value"], "nak" not in w.get("flags", [])) for w in ws])

bad = 0
for i, (a, b) in enumerate(zip(ref, ours)):
    if a != b:
        bad += 1
        if bad <= 5:
            print("segment", i, "differs:", a, b)
print(f"reference {len(ref)} segments, bus-replay {len(ours)} segments, {bad} differ")
sys.exit(0 if bad == 0 and len(ref) == len(ours) else 1)
