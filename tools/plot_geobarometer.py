#!/usr/bin/env python3
"""Figures of easyMelts geobarometer runs, from the summary and detail CSV files.

Part of the easyMelts geobarometer, GNU General Public License version 3.

    python plot_geobarometer.py TEC                      # TEC_summary.csv + TEC_detail.csv -> TEC_runs.pdf
    python plot_geobarometer.py TEC --runs flagged       # only the runs with a screening flag
    python plot_geobarometer.py TEC --figures figs --format svg --runs "TEC-210-43"
    python plot_geobarometer.py TEC --ensemble           # TEC_ensemble.pdf: one page per glass

Each run is drawn as in the Geobarometer tab: the saturation temperatures of the three phases (and the
wet liquidus) against pressure, and the residuals with the fitted parabolas and their vertices. The
flags of the summary are printed under the title. The parabolas come from the fit columns of the
summary (release 4 on); for older files they are fitted again to the same five points.
Needs numpy and matplotlib.
"""
import argparse
import csv
import math
import os
import re
import sys

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.backends.backend_pdf import PdfPages  # noqa: E402

PHASE_COLORS = ("#2a78d6", "#eb6834", "#1baf7a")
FIT_COLORS = ("#184d8f", "#a8401a")
INK, MUTED, GRID = "#1f1f1f", "#5f5e5a", "#e2e1dc"


def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return math.nan


def read_runs(summary_path, detail_path):
    with open(summary_path, newline="", encoding="utf-8-sig") as f:
        summary = list(csv.DictReader(f))
    with open(detail_path, newline="", encoding="utf-8-sig") as f:
        reader = csv.reader(f)
        head = next(reader)
        rows = [r + [""] * (len(head) - len(r)) for r in reader if r]
    ix = {name: i for i, name in enumerate(head)}
    liq = ix["wet_liquidus_C"]
    detail, occurrence, prev = {}, {}, None
    for r in rows:  # rows of one run are consecutive; repeated (sample, offset) pairs are counted
        key = (r[ix["sample"]], r[ix["fO2_offset"]])
        if key != prev:
            occurrence[key] = occurrence.get(key, 0) + 1
        prev = key
        detail.setdefault(key + (occurrence[key],), []).append(r)
    seen, runs = {}, []
    for s in summary:
        key = (s["sample"], s["fO2_offset"])
        seen[key] = seen.get(key, 0) + 1
        d = detail.get(key + (seen[key],), [])
        run = {"s": s, "P": np.array([number(r[ix["P_MPa"]]) for r in d]),
               "liq": np.array([number(r[liq]) for r in d]),
               "T": [np.array([number(r[liq + 1 + k]) for r in d]) for k in range(3)],
               "d3": np.array([number(r[ix["delta_3"]]) for r in d]),
               "d2": np.array([number(r[ix["delta_2"]]) for r in d]),
               "others": {h[5:]: np.array([number(r[i]) for r in d]) for h, i in ix.items() if h.startswith("Tsat_")}}
        run["phases"] = [s.get("phase%d" % (k + 1), "phase %d" % (k + 1)) for k in range(3)]
        runs.append(run)
    return runs


def parabola(run, kind):
    """(a, b, c, P from, P to, vertex) of the 3-phase or 2-phase fit, or None."""
    s = run["s"]
    p_est = number(s.get("P_%s_MPa" % kind))
    a = number(s.get("fit_a_%s" % kind))
    if not math.isnan(a):
        b, c = number(s["fit_b_%s" % kind]), number(s["fit_c_%s" % kind])
        lo, hi = number(s["fit_P_min_%s_MPa" % kind]), number(s["fit_P_max_%s_MPa" % kind])
    else:  # files written before the fit columns: the same least-squares fit on the same points
        P, r = run["P"], run["d3" if kind == "3phase" else "d2"]
        pm = number(s.get("P_at_min_%s_MPa" % kind))
        if math.isnan(p_est) or len(P) == 0 or math.isnan(pm):
            return None
        i = int(np.argmin(np.abs(P - pm)))
        sel = slice(max(i - 2, 0), min(i + 2, len(P) - 1) + 1)
        if np.isnan(r[sel]).any() or len(P[sel]) < 3:
            return None
        a, b, c = np.polyfit(P[sel], r[sel], 2)
        lo, hi = float(np.min(P[sel])), float(np.max(P[sel]))
    if not a > 0:
        return None
    if not math.isnan(p_est):
        lo, hi = min(lo, p_est), max(hi, p_est)
    return a, b, c, lo, hi, p_est


def flags(run):
    out = []
    for kind, tag in (("3phase", "P3"), ("2phase", "P2")):
        for x in filter(None, (y.strip() for y in run["s"].get("flags_" + kind, "").split(";"))):
            out.append("%s: %s" % (tag, x))
    return out


def label(run):
    s = run["s"]
    off = number(s["fO2_offset"])
    return "%s (%+.2f)" % (s["sample"], off) if not math.isnan(off) else s["sample"]


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.7)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.tick_params(colors=MUTED, labelsize=8.5)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#b9b8b2")


def draw_run(run, fig, others=False, text=True):
    s, P = run["s"], run["P"]
    if not len(P):  # refused, or not in the detail file
        ran = s.get("equilibrations") not in ("", "0", None)
        why = "calculated, but not in the detail file" if ran else (s.get("notes") or "no result")
        fig.text(0.02, 0.6, label(run), fontsize=11, color=INK)
        fig.text(0.02, 0.45, why, fontsize=9, color=MUTED, wrap=True)
        return
    ax1, ax2 = fig.subplots(1, 2)
    order = np.argsort(P) if len(P) else []
    for k in range(3):
        T = run["T"][k]
        ax1.plot(P[order], T[order], "-o", color=PHASE_COLORS[k], lw=1.4, ms=3.2, label=run["phases"][k])
    if np.isfinite(run["liq"]).any():
        ax1.plot(P[order], run["liq"][order], "--", color="#8a8a8a", lw=1.0, label="wet liquidus")
    if others:
        chosen = set(run["phases"])
        for name, T in sorted(run["others"].items()):
            if name in chosen or re.sub(r"\d+$", "", name) in ("water", "fluid") or not np.isfinite(T).any():
                continue
            ax1.plot(P[order], T[order], "-", color="#c9c8c2", lw=0.8)
            j = int(np.nanargmax(np.where(np.isfinite(T[order]), P[order], -np.inf)))
            ax1.annotate(name, (P[order][j], T[order][j]), fontsize=6.5, color=MUTED, xytext=(3, 0), textcoords="offset points")
    ax1.set_xlabel("P (MPa)")
    ax1.set_ylabel("T (°C)")
    ax1.set_title("Saturation temperatures", fontsize=10, color=INK)
    ax1.legend(fontsize=7.5, frameon=False)

    for kind, r, k, name in (("3phase", run["d3"], 0, "3 phases"), ("2phase", run["d2"], 1, "2 phases")):
        ax2.plot(P[order], r[order], "-o", color=PHASE_COLORS[k], lw=1.1, ms=3.2, label="residual, " + name)
        fit = parabola(run, kind)
        if fit:
            a, b, c, lo, hi, p_est = fit
            x = np.linspace(lo, hi, 61)
            ax2.plot(x, a * x * x + b * x + c, "-", color=FIT_COLORS[k], lw=2.2, label="fit, " + name)
            if not math.isnan(p_est):
                ax2.plot([p_est], [a * p_est ** 2 + b * p_est + c], "D", color=FIT_COLORS[k], mec="white", mew=0.8, ms=7,
                         label="P%s = %.1f MPa" % ("3" if kind == "3phase" else "2", p_est))
    thr = number(s.get("threshold_C"))
    if not math.isnan(thr):
        ax2.axhline(thr, color="#8a8a8a", lw=0.8, ls=":")
    ax2.set_ylim(bottom=0)
    ax2.set_xlabel("P (MPa)")
    ax2.set_ylabel("residual (°C)")
    ax2.set_title("Residuals and parabola fits", fontsize=10, color=INK)
    ax2.legend(fontsize=7.5, frameon=False)
    for ax in (ax1, ax2):
        style(ax)
        if len(P):
            ax.set_xlim(min(0, np.nanmin(P)), np.nanmax(P) * 1.02)
    if text:
        head = label(run)
        fig.suptitle(head, x=0.01, ha="left", fontsize=11, color=INK, y=0.995)
        f = flags(run)
        if f:
            fig.text(0.01, 0.935, "; ".join(f), fontsize=7.5, color="#a8401a", ha="left", va="top", wrap=True)
    fig.tight_layout(rect=(0, 0, 1, 0.9 if text else 1))


def group_of(sample):
    m = re.match(r"^(.*)-\d+$", sample)
    return m.group(1) if m else sample


def draw_ensemble(name, runs, fig):
    ax1, ax2 = fig.subplots(1, 2, gridspec_kw={"width_ratios": [1.6, 1]})
    p3, p2 = [], []
    for run in runs:
        P = run["P"]
        if len(P):
            o = np.argsort(P)
            ax1.plot(P[o], run["d3"][o], "-", color=PHASE_COLORS[0], lw=0.6, alpha=0.18)
        v3, v2 = number(run["s"].get("P_3phase_MPa")), number(run["s"].get("P_2phase_MPa"))
        if not math.isnan(v3):
            p3.append(v3)
        if not math.isnan(v2):
            p2.append(v2)
    if p3:  # where each draw's three-phase pressure falls
        ax1.plot(p3, np.full(len(p3), 0.6), "|", color=FIT_COLORS[0], ms=9, alpha=0.35)
    ax1.set_ylim(0, 40)
    ax1.set_xlabel("P (MPa)")
    ax1.set_ylabel("3-phase residual (°C)")
    ax1.set_title("Residual curves of every draw", fontsize=10, color=INK)
    style(ax1)
    rng = np.random.default_rng(1)
    for k, (vals, tag) in enumerate(((p3, "P3"), (p2, "P2"))):
        if vals:
            y = k + rng.uniform(-0.18, 0.18, len(vals))
            ax2.scatter(vals, y, s=7, color=PHASE_COLORS[k], alpha=0.45, lw=0)
            med = float(np.median(vals))
            ax2.plot([med, med], [k - 0.3, k + 0.3], color=FIT_COLORS[k], lw=2.5)
            ax2.text(med, k + 0.36, "%s median %.0f MPa (%d draws)" % (tag, med, len(vals)), ha="center", fontsize=7.5, color=INK)
    ax2.set_yticks([0, 1])
    ax2.set_yticklabels(["P3", "P2"])
    ax2.set_ylim(-0.6, 1.8)
    ax2.set_xlabel("P (MPa)")
    ax2.set_title("Pressures", fontsize=10, color=INK)
    style(ax2)
    nflag = sum(1 for r in runs if flags(r))
    fig.suptitle("%s: %d draws, %d with a flag" % (name, len(runs), nflag), x=0.01, ha="left", fontsize=11, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.94))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("prefix", help="name of the run (PREFIX_summary.csv and PREFIX_detail.csv), or the summary file")
    ap.add_argument("--detail", help="detail file, if not PREFIX_detail.csv")
    ap.add_argument("--runs", default="all", help="all, flagged, or sample names separated by commas")
    ap.add_argument("--book", help="PDF with one run per page (default PREFIX_runs.pdf)")
    ap.add_argument("--figures", help="folder for one figure file per run")
    ap.add_argument("--format", default="png", choices=("png", "pdf", "svg"), help="format of --figures (default png)")
    ap.add_argument("--ensemble", action="store_true", help="PREFIX_ensemble.pdf: draws grouped by name without the final -N")
    ap.add_argument("--others", action="store_true", help="also draw the other phases that appeared (grey)")
    ap.add_argument("--plain", action="store_true", help="no title or flags on the figures (for publication)")
    args = ap.parse_args()

    prefix = re.sub(r"_summary\.csv$", "", args.prefix)
    summary = args.prefix if args.prefix.endswith("_summary.csv") else prefix + "_summary.csv"
    detail = args.detail or prefix + "_detail.csv"
    runs = read_runs(summary, detail)
    if args.runs == "flagged":
        chosen = [r for r in runs if flags(r)]
    elif args.runs == "all":
        chosen = runs
    else:
        names = {x.strip() for x in args.runs.split(",")}
        chosen = [r for r in runs if r["s"]["sample"] in names or label(r) in names]
    print("%d runs in %s, %d chosen" % (len(runs), summary, len(chosen)))

    if args.ensemble:
        groups = {}
        for r in runs:
            groups.setdefault(group_of(r["s"]["sample"]), []).append(r)
        out = prefix + "_ensemble.pdf"
        with PdfPages(out) as pdf:
            for name, rs in groups.items():
                if len(rs) < 2:
                    continue
                fig = plt.figure(figsize=(11, 4.2))
                draw_ensemble(name, rs, fig)
                pdf.savefig(fig)
                plt.close(fig)
        print("wrote", out)
    if args.figures:
        os.makedirs(args.figures, exist_ok=True)
        for r in chosen:
            fig = plt.figure(figsize=(10, 4.2))
            draw_run(r, fig, args.others, not args.plain)
            name = re.sub(r"[^A-Za-z0-9._+-]+", "_", label(r)).strip("_")
            fig.savefig(os.path.join(args.figures, name + "." + args.format), dpi=200)
            plt.close(fig)
        print("wrote %d figures in %s" % (len(chosen), args.figures))
    if not args.ensemble and not args.figures or args.book:
        out = args.book or prefix + "_runs.pdf"
        if len(chosen) > 300:
            print("%d pages; --runs flagged keeps only the flagged runs" % len(chosen))
        with PdfPages(out) as pdf:
            for r in chosen:
                fig = plt.figure(figsize=(11, 4.6))
                draw_run(r, fig, args.others, not args.plain)
                pdf.savefig(fig)
                plt.close(fig)
        print("wrote", out)


if __name__ == "__main__":
    sys.exit(main())
