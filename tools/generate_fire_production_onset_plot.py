#!/usr/bin/env python3
"""Generate the deterministic r181 onset trajectory and term-budget artifacts."""

import argparse
import csv
import hashlib
import math
from pathlib import Path


TERMS = (
    ("stress", "column_stress_rate_max", "#4c78a8"),
    ("buoyancy", "column_buoyancy_rate_max", "#f58518"),
    ("advection", "column_advection_rate_max", "#e45756"),
    ("pressure", "column_pressure_rate_max", "#72b7b2"),
    ("restoration", "column_restoration_rate_max", "#b279a2"),
)


def rows(path):
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def aligned_budget(path, select_maximum=False):
    summaries = rows(path)
    summary = (max(summaries, key=lambda row: float(row["attempt_velocity_max_m_per_s"]))
               if select_maximum else summaries[-1])
    time = float(summary["beginning_time_s"])
    candidate = int(summary["candidate"])
    z_face = int(summary["attempt_velocity_z"])
    candidates = rows(Path(str(path)+".column.csv"))
    column = min((row for row in candidates
                  if int(row["candidate"]) == candidate and int(row["z_face"]) == z_face),
                 key=lambda row: abs(float(row["beginning_time_s"])-time))
    return {**summary, **column,
            "attempt_velocity_max_m_per_s": summary["attempt_velocity_max_m_per_s"],
            "column_stress_rate_max": column["stress_rate"],
            "column_buoyancy_rate_max": column["buoyancy_rate"],
            "column_advection_rate_max": column["advection_rate"],
            "column_source_rate_max": column["source_rate"],
            "column_pressure_rate_max": column["pressure_gradient_rate"],
            "column_restoration_rate_max": column["restoration_rate"],
            "column_vreman_min_m2_per_s": str(min(float(column["lower_vreman_m2_per_s"]),
                float(column["upper_vreman_m2_per_s"]))),
            "column_vreman_max_m2_per_s": str(max(float(column["lower_vreman_m2_per_s"]),
                float(column["upper_vreman_m2_per_s"])))}


def escape(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def line(points, color, width=3):
    return (f'<polyline fill="none" stroke="{color}" stroke-width="{width}" '
            f'points="{" ".join(f"{x:.3f},{y:.3f}" for x, y in points)}"/>')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier6-trajectory", type=Path, required=True)
    parser.add_argument("--intermediate-trajectory", type=Path)
    parser.add_argument("--tier10-trajectory", type=Path, action="append", required=True)
    parser.add_argument("--tier6-budget", type=Path, required=True)
    parser.add_argument("--intermediate-budget", type=Path)
    parser.add_argument("--threshold-budget", type=Path, action="append", required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--svg", type=Path, required=True)
    args = parser.parse_args()

    tier6 = rows(args.tier6_trajectory)
    intermediate = rows(args.intermediate_trajectory) if args.intermediate_trajectory else []
    tier10_by_step = {}
    for path in args.tier10_trajectory:
        for row in rows(path):
            tier10_by_step[int(row["accepted_step"])] = row
    tier10 = [tier10_by_step[key] for key in sorted(tier10_by_step)]
    tier6_budget = aligned_budget(args.tier6_budget, True)
    intermediate_budget = (aligned_budget(args.intermediate_budget, True)
                           if args.intermediate_budget else None)
    threshold_rows = [aligned_budget(path) for path in args.threshold_budget]
    budget_rows = [("tier6 late", tier6_budget)]
    if intermediate_budget:
        budget_rows.append(("tier8 pre-bound", intermediate_budget))
    budget_rows += [
        (f'{float(row["attempt_velocity_max_m_per_s"]):.0f} m/s crossing', row)
        for row in threshold_rows
    ]

    args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.summary.open("w", newline="", encoding="utf-8") as target:
        fieldnames = ["regime", "time_s", "dt_s", "velocity_m_per_s"] + [name for name, _, _ in TERMS] + [
            "vreman_min_m2_per_s", "vreman_max_m2_per_s"]
        writer = csv.DictWriter(target, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for label, row in budget_rows:
            writer.writerow({
                "regime": label,
                "time_s": row["beginning_time_s"],
                "dt_s": row["dt_s"],
                "velocity_m_per_s": row["attempt_velocity_max_m_per_s"],
                **{name: row[field] for name, field, _ in TERMS},
                "vreman_min_m2_per_s": row["column_vreman_min_m2_per_s"],
                "vreman_max_m2_per_s": row["column_vreman_max_m2_per_s"],
            })

    width, height = 1200, 820
    left, right = 90, 1160
    top0, bottom0 = 75, 400
    top1, bottom1 = 500, 765
    def trajectory_point(row):
        x = left + (right-left)*float(row["time_s"])/2.2
        y = bottom0 - (bottom0-top0)*float(row["maximum_velocity_m_per_s"])/70.0
        return x, y
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#fbfbf8"/>',
        '<style>text{font-family:Helvetica,Arial,sans-serif;fill:#202124}.title{font-size:24px;font-weight:700}.axis{font-size:14px}.small{font-size:12px}</style>',
        '<text class="title" x="90" y="38">r181 production onset: resolution trajectory and column momentum budget</text>']
    for velocity in range(0, 71, 10):
        y = bottom0 - (bottom0-top0)*velocity/70.0
        svg.append(f'<line x1="{left}" y1="{y:.3f}" x2="{right}" y2="{y:.3f}" stroke="#d8d8d2"/>')
        svg.append(f'<text class="axis" x="{left-12}" y="{y+5:.3f}" text-anchor="end">{velocity}</text>')
    for time in (0.0, 0.5, 1.0, 1.5, 2.0, 2.2):
        x = left + (right-left)*time/2.2
        svg.append(f'<line x1="{x:.3f}" y1="{top0}" x2="{x:.3f}" y2="{bottom0}" stroke="#e5e5df"/>')
        svg.append(f'<text class="axis" x="{x:.3f}" y="{bottom0+23}" text-anchor="middle">{time:g}</text>')
    svg += [line([trajectory_point(row) for row in tier6], "#4c78a8")]
    if intermediate:
        svg.append(line([trajectory_point(row) for row in intermediate], "#54a24b"))
    svg += [line([trajectory_point(row) for row in tier10], "#e45756"),
        f'<line x1="{left}" y1="{bottom0}" x2="{right}" y2="{bottom0}" stroke="#202124"/>',
        f'<line x1="{left}" y1="{top0}" x2="{left}" y2="{bottom0}" stroke="#202124"/>',
        f'<text class="axis" x="{(left+right)/2}" y="{bottom0+48}" text-anchor="middle">simulation time (s)</text>',
        f'<text class="axis" x="24" y="{(top0+bottom0)/2}" text-anchor="middle" transform="rotate(-90 24 {(top0+bottom0)/2})">max |u| (m/s)</text>',
        '<line x1="820" y1="92" x2="860" y2="92" stroke="#4c78a8" stroke-width="3"/><text class="axis" x="870" y="97">tier 6</text>',
        '<line x1="930" y1="92" x2="970" y2="92" stroke="#54a24b" stroke-width="3"/><text class="axis" x="980" y="97">tier 8</text>',
        '<line x1="1040" y1="92" x2="1080" y2="92" stroke="#e45756" stroke-width="3"/><text class="axis" x="1090" y="97">tier 10</text>']

    max_rate = 18000.0
    zero_y = top1 + (bottom1-top1)*0.90
    rate_scale = (bottom1-top1)*0.80/max_rate
    svg.append(f'<line x1="{left}" y1="{zero_y:.3f}" x2="{right}" y2="{zero_y:.3f}" stroke="#202124"/>')
    group_width = (right-left)/len(budget_rows)
    bar_width = group_width/(len(TERMS)+2)
    for group, (label, row) in enumerate(budget_rows):
        center = left + group_width*(group+0.5)
        for term, (_, field, color) in enumerate(TERMS):
            value = float(row[field])
            x = center + (term-(len(TERMS)-1)/2)*bar_width
            y = zero_y - value*rate_scale
            svg.append(f'<rect x="{x-bar_width*0.38:.3f}" y="{min(y,zero_y):.3f}" width="{bar_width*0.76:.3f}" height="{abs(y-zero_y):.3f}" fill="{color}"/>')
        svg.append(f'<text class="axis" x="{center:.3f}" y="{bottom1+22}" text-anchor="middle">{escape(label)}</text>')
        svg.append(f'<text class="small" x="{center:.3f}" y="{bottom1+40}" text-anchor="middle">Vreman max {float(row["column_vreman_max_m2_per_s"]):.3g} m²/s</text>')
    for rate in (0, 5000, 10000, 15000):
        y = zero_y-rate*rate_scale
        svg.append(f'<line x1="{left}" y1="{y:.3f}" x2="{right}" y2="{y:.3f}" stroke="#deded8"/>')
        svg.append(f'<text class="axis" x="{left-12}" y="{y+5:.3f}" text-anchor="end">{rate}</text>')
    svg.append(f'<text class="axis" x="24" y="{(top1+bottom1)/2}" text-anchor="middle" transform="rotate(-90 24 {(top1+bottom1)/2})">signed dM/dt term (kg/(m² s²))</text>')
    legend_x = 735
    for index, (name, _, color) in enumerate(TERMS):
        x = legend_x + (index % 3)*145
        y = 475 + (index // 3)*20
        svg.append(f'<rect x="{x}" y="{y-11}" width="12" height="12" fill="{color}"/><text class="small" x="{x+18}" y="{y}">{name}</text>')
    svg.append('</svg>')
    args.svg.parent.mkdir(parents=True, exist_ok=True)
    args.svg.write_text("\n".join(svg)+"\n", encoding="utf-8")
    print("summary_sha256", hashlib.sha256(args.summary.read_bytes()).hexdigest())
    print("svg_sha256", hashlib.sha256(args.svg.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
