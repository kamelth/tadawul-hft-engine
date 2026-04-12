#!/usr/bin/env python3
"""
plot_impact.py - Visualize baseline vs HFT market quality comparison.

Reads:
    results/baseline_snapshots.csv
    results/hft_snapshots.csv

Produces:
    results/spread_comparison.png
    results/depth_comparison.png

Usage:
    python scripts/plot_impact.py [results_dir]

Dependencies: matplotlib (pip install matplotlib).
"""

import csv
import os
import sys
from collections import defaultdict


def read_snapshots(path):
    snapshots = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            snapshots.append({
                "timestamp_ns": int(row["timestamp_ns"]),
                "symbol": row["symbol"],
                "best_bid": int(row["best_bid"]),
                "best_ask": int(row["best_ask"]),
                "spread": int(row["spread"]),
                "bid_volume": int(row["bid_volume"]),
                "ask_volume": int(row["ask_volume"]),
                "bid_levels": int(row["bid_levels"]),
                "ask_levels": int(row["ask_levels"]),
                "total_orders": int(row["total_orders"]),
            })
    return snapshots


def ns_to_market_time(ns):
    """Convert nanoseconds since midnight to HH:MM:SS string."""
    total_sec = ns / 1e9
    h = int(total_sec // 3600)
    m = int((total_sec % 3600) // 60)
    s = int(total_sec % 60)
    return f"{h:02d}:{m:02d}:{s:02d}"


def ns_to_hours(ns):
    """Convert nanoseconds since midnight to fractional hours."""
    return ns / 3.6e12


def aggregate_by_timestamp(snapshots):
    """Average spread/depth across all symbols at each timestamp."""
    by_ts = defaultdict(list)
    for s in snapshots:
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            by_ts[s["timestamp_ns"]].append(s)

    timestamps = sorted(by_ts.keys())
    result = {"ts": [], "avg_spread": [], "total_depth": [], "total_orders": []}
    for ts in timestamps:
        snaps = by_ts[ts]
        n = len(snaps)
        result["ts"].append(ts)
        result["avg_spread"].append(sum(s["spread"] for s in snaps) / n / 10000.0)
        result["total_depth"].append(
            sum(s["bid_volume"] + s["ask_volume"] for s in snaps) / n
        )
        result["total_orders"].append(sum(s["total_orders"] for s in snaps) / n)
    return result


def per_symbol_avg_spread(snapshots):
    """Return {symbol: avg_spread_dollars} for valid snapshots."""
    by_sym = defaultdict(list)
    for s in snapshots:
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            by_sym[s["symbol"]].append(s["spread"])
    return {sym: sum(v) / len(v) / 10000.0 for sym, v in by_sym.items()}


def per_symbol_avg_depth(snapshots):
    by_sym = defaultdict(list)
    for s in snapshots:
        if s["best_bid"] > 0 and s["best_ask"] > 0:
            by_sym[s["symbol"]].append(s["bid_volume"] + s["ask_volume"])
    return {sym: sum(v) / len(v) for sym, v in by_sym.items()}


def plot_spread(results_dir, baseline, hft):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping plots")
        return False

    b_agg = aggregate_by_timestamp(baseline)
    h_agg = aggregate_by_timestamp(hft)

    fig, axes = plt.subplots(2, 2, figsize=(14, 9))

    # 1) Spread over time
    ax = axes[0, 0]
    b_hours = [ns_to_hours(t) for t in b_agg["ts"]]
    h_hours = [ns_to_hours(t) for t in h_agg["ts"]]
    ax.plot(b_hours, b_agg["avg_spread"], label="Baseline", linewidth=1.5, alpha=0.8)
    ax.plot(h_hours, h_agg["avg_spread"], label="HFT", linewidth=1.5, alpha=0.8)
    ax.set_xlabel("Market time (hours)")
    ax.set_ylabel("Avg spread ($)")
    ax.set_title("Bid-Ask Spread Over Time")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # 2) Per-symbol spread bar chart
    ax = axes[0, 1]
    b_sym_spread = per_symbol_avg_spread(baseline)
    h_sym_spread = per_symbol_avg_spread(hft)
    symbols = sorted(set(list(b_sym_spread.keys()) + list(h_sym_spread.keys())))
    x = range(len(symbols))
    width = 0.35
    b_vals = [b_sym_spread.get(s, 0) for s in symbols]
    h_vals = [h_sym_spread.get(s, 0) for s in symbols]
    ax.bar([i - width / 2 for i in x], b_vals, width, label="Baseline", alpha=0.8)
    ax.bar([i + width / 2 for i in x], h_vals, width, label="HFT", alpha=0.8)
    ax.set_xticks(list(x))
    ax.set_xticklabels(symbols, rotation=45, ha="right")
    ax.set_ylabel("Avg spread ($)")
    ax.set_title("Per-Symbol Average Spread")
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)

    # 3) Depth over time
    ax = axes[1, 0]
    ax.plot(b_hours, b_agg["total_depth"], label="Baseline", linewidth=1.5, alpha=0.8)
    ax.plot(h_hours, h_agg["total_depth"], label="HFT", linewidth=1.5, alpha=0.8)
    ax.set_xlabel("Market time (hours)")
    ax.set_ylabel("Avg depth (shares)")
    ax.set_title("Order Book Depth Over Time")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # 4) Per-symbol depth bar chart
    ax = axes[1, 1]
    b_sym_depth = per_symbol_avg_depth(baseline)
    h_sym_depth = per_symbol_avg_depth(hft)
    b_dvals = [b_sym_depth.get(s, 0) for s in symbols]
    h_dvals = [h_sym_depth.get(s, 0) for s in symbols]
    ax.bar([i - width / 2 for i in x], b_dvals, width, label="Baseline", alpha=0.8)
    ax.bar([i + width / 2 for i in x], h_dvals, width, label="HFT", alpha=0.8)
    ax.set_xticks(list(x))
    ax.set_xticklabels(symbols, rotation=45, ha="right")
    ax.set_ylabel("Avg depth (shares)")
    ax.set_title("Per-Symbol Average Depth")
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)

    fig.suptitle("Market Impact: Baseline vs HFT Market Maker", fontsize=14)
    fig.tight_layout()

    out = os.path.join(results_dir, "spread_comparison.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"Wrote {out}")
    return True


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "results"

    baseline_path = os.path.join(results_dir, "baseline_snapshots.csv")
    hft_path = os.path.join(results_dir, "hft_snapshots.csv")

    for p, name in [(baseline_path, "baseline"), (hft_path, "hft")]:
        if not os.path.exists(p):
            print(f"Error: {p} not found. Run {name} simulation first.",
                  file=sys.stderr)
            sys.exit(1)

    baseline = read_snapshots(baseline_path)
    hft = read_snapshots(hft_path)

    if not baseline or not hft:
        print("Error: empty snapshot files.", file=sys.stderr)
        sys.exit(1)

    plotted = plot_spread(results_dir, baseline, hft)
    if not plotted:
        print("No charts generated (matplotlib missing?).")
        sys.exit(1)


if __name__ == "__main__":
    main()
