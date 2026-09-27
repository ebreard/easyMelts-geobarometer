"""Compare a geobarometer_cli run with the reference table (easyMelts geobarometer tests).

   python3 geobarometer_compare.py run_detail.csv run_summary.csv reference_detail.csv

Saturation temperatures are compared phase by phase at every pressure; the run passes if they
all agree within 1 C and both pressures (from the summary) are within 1 MPa of the reference
values 377.3 and 372.5 MPa (Bishop Tuff, NNO, default settings).
"""
import csv
import sys

REFERENCE_P3, REFERENCE_P2 = 377.3, 372.5


def rows(path):
    with open(path, newline="") as f:
        return {float(r["P_MPa"]): r for r in csv.DictReader(f)}


run, ref = rows(sys.argv[1]), rows(sys.argv[3])
with open(sys.argv[2], newline="") as f:
    summary = next(csv.DictReader(f))

compared = same = 0
worst = 0.0
for p, r in sorted(ref.items(), reverse=True):
    if p not in run:
        print("pressure %.0f MPa missing from the run" % p)
        worst = max(worst, 99.0)
        continue
    for key, value in r.items():
        if not (key.startswith("T_") or key.startswith("Tsat_")) or value == "":
            continue
        other = run[p].get(key, "")
        compared += 1
        if other == "":
            print("%5.0f MPa %-24s reference %s, run absent" % (p, key, value))
            worst = max(worst, 99.0)
            continue
        d = abs(float(other) - float(value))
        worst = max(worst, d)
        if d == 0.0:
            same += 1
        else:
            print("%5.0f MPa %-24s reference %s, run %s" % (p, key, value, other))

p3 = float(summary["P_3phase_MPa"] or "nan")
p2 = float(summary["P_2phase_MPa"] or "nan")
print("saturation temperatures identical: %d of %d, largest difference %.1f C" % (same, compared, worst))
print("P3 %.1f MPa (reference %.1f), P2 %.1f MPa (reference %.1f)" % (p3, REFERENCE_P3, p2, REFERENCE_P2))
ok = worst <= 1.0 and abs(p3 - REFERENCE_P3) <= 1.0 and abs(p2 - REFERENCE_P2) <= 1.0
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
