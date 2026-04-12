#!/usr/bin/env python3
"""
analyze_impact.py - Compare baseline vs HFT market quality metrics.

Reads:
    results/baseline_snapshots.csv
    results/hft_snapshots.csv

Produces:
    results/market_impact_report.txt

Usage:
    python scripts/analyze_impact.py [results_dir]
"""

import csv
import os
import sys
from collections import defaultdict


def read_snapshots(path):
    """Read snapshots CSV into list of dicts, grouped by timestamp."""
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


def compute_stats(snapshots):
    """Compute aggregate statistics from snapshot list."""
    # Filter out snapshots with no valid market (bid or ask = 0)
    valid = [s for s in snapshots if s["best_bid"] > 0 and s["best_ask"] > 0]
    if not valid:
        return None

    spreads = [s["spread"] for s in valid]
    bid_vols = [s["bid_volume"] for s in valid]
    ask_vols = [s["ask_volume"] for s in valid]
    bid_levels = [s["bid_levels"] for s in valid]
    ask_levels = [s["ask_levels"] for s in valid]
    orders = [s["total_orders"] for s in valid]

    spreads_sorted = sorted(spreads)
    n = len(spreads_sorted)

    def percentile(data, p):
        k = (len(data) - 1) * p / 100.0
        f = int(k)
        c = f + 1 if f + 1 < len(data) else f
        return data[f] + (data[c] - data[f]) * (k - f)

    return {
        "count": n,
        "avg_spread": sum(spreads) / n,
        "median_spread": percentile(spreads_sorted, 50),
        "p25_spread": percentile(spreads_sorted, 25),
        "p75_spread": percentile(spreads_sorted, 75),
        "min_spread": min(spreads),
        "max_spread": max(spreads),
        "avg_bid_volume": sum(bid_vols) / n,
        "avg_ask_volume": sum(ask_vols) / n,
        "avg_bid_levels": sum(bid_levels) / n,
        "avg_ask_levels": sum(ask_levels) / n,
        "avg_total_orders": sum(orders) / n,
        "total_depth": (sum(bid_vols) + sum(ask_vols)) / n,
    }


def compute_per_symbol(snapshots):
    """Compute per-symbol statistics."""
    by_symbol = defaultdict(list)
    for s in snapshots:
        by_symbol[s["symbol"]].append(s)
    result = {}
    for symbol, snaps in sorted(by_symbol.items()):
        stats = compute_stats(snaps)
        if stats:
            result[symbol] = stats
    return result


def format_price(price_units):
    """Convert $0.0001 units to dollars."""
    return f"${price_units / 10000:.4f}"


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "results"

    baseline_path = os.path.join(results_dir, "baseline_snapshots.csv")
    hft_path = os.path.join(results_dir, "hft_snapshots.csv")

    if not os.path.exists(baseline_path):
        print(f"Error: {baseline_path} not found. Run hft_engine first.", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(hft_path):
        print(f"Error: {hft_path} not found. Run hft_strategy first.", file=sys.stderr)
        sys.exit(1)

    baseline = read_snapshots(baseline_path)
    hft = read_snapshots(hft_path)

    baseline_stats = compute_stats(baseline)
    hft_stats = compute_stats(hft)

    if not baseline_stats or not hft_stats:
        print("Error: insufficient data for comparison.", file=sys.stderr)
        sys.exit(1)

    baseline_by_sym = compute_per_symbol(baseline)
    hft_by_sym = compute_per_symbol(hft)

    # Build report
    lines = []
    lines.append("=" * 70)
    lines.append("MARKET IMPACT ANALYSIS REPORT")
    lines.append("=" * 70)
    lines.append("")
    lines.append(f"Baseline snapshots:  {baseline_stats['count']}")
    lines.append(f"HFT snapshots:       {hft_stats['count']}")
    lines.append("")

    # Aggregate comparison
    lines.append("-" * 70)
    lines.append("AGGREGATE COMPARISON (all symbols, all time)")
    lines.append("-" * 70)
    lines.append(f"{'Metric':<30} {'Baseline':>15} {'HFT':>15} {'Change':>10}")
    lines.append("-" * 70)

    def compare_row(label, b_val, h_val, fmt="price"):
        if fmt == "price":
            b_str = format_price(b_val)
            h_str = format_price(h_val)
        elif fmt == "float":
            b_str = f"{b_val:.1f}"
            h_str = f"{h_val:.1f}"
        else:
            b_str = str(b_val)
            h_str = str(h_val)

        if b_val > 0:
            pct = ((h_val - b_val) / b_val) * 100
            change = f"{pct:+.1f}%"
        else:
            change = "N/A"
        lines.append(f"{label:<30} {b_str:>15} {h_str:>15} {change:>10}")

    compare_row("Avg spread", baseline_stats["avg_spread"], hft_stats["avg_spread"])
    compare_row("Median spread", baseline_stats["median_spread"], hft_stats["median_spread"])
    compare_row("Avg bid volume", baseline_stats["avg_bid_volume"], hft_stats["avg_bid_volume"], "float")
    compare_row("Avg ask volume", baseline_stats["avg_ask_volume"], hft_stats["avg_ask_volume"], "float")
    compare_row("Total depth (bid+ask)", baseline_stats["total_depth"], hft_stats["total_depth"], "float")
    compare_row("Avg bid levels", baseline_stats["avg_bid_levels"], hft_stats["avg_bid_levels"], "float")
    compare_row("Avg ask levels", baseline_stats["avg_ask_levels"], hft_stats["avg_ask_levels"], "float")
    compare_row("Avg total orders", baseline_stats["avg_total_orders"], hft_stats["avg_total_orders"], "float")

    lines.append("")

    # Per-symbol comparison
    lines.append("-" * 70)
    lines.append("PER-SYMBOL SPREAD COMPARISON")
    lines.append("-" * 70)
    lines.append(f"{'Symbol':<10} {'Baseline Avg':>15} {'HFT Avg':>15} {'Change':>10} {'B Depth':>10} {'H Depth':>10}")
    lines.append("-" * 70)

    all_symbols = sorted(set(list(baseline_by_sym.keys()) + list(hft_by_sym.keys())))
    for sym in all_symbols:
        b = baseline_by_sym.get(sym)
        h = hft_by_sym.get(sym)
        if not b or not h:
            continue
        b_spread = format_price(b["avg_spread"])
        h_spread = format_price(h["avg_spread"])
        if b["avg_spread"] > 0:
            pct = ((h["avg_spread"] - b["avg_spread"]) / b["avg_spread"]) * 100
            change = f"{pct:+.1f}%"
        else:
            change = "N/A"
        b_depth = f"{b['total_depth']:.0f}"
        h_depth = f"{h['total_depth']:.0f}"
        lines.append(f"{sym:<10} {b_spread:>15} {h_spread:>15} {change:>10} {b_depth:>10} {h_depth:>10}")

    lines.append("")
    lines.append("=" * 70)
    lines.append("INTERPRETATION")
    lines.append("=" * 70)

    # Auto-interpret
    spread_change = ((hft_stats["avg_spread"] - baseline_stats["avg_spread"])
                     / baseline_stats["avg_spread"] * 100) if baseline_stats["avg_spread"] > 0 else 0
    depth_change = ((hft_stats["total_depth"] - baseline_stats["total_depth"])
                    / baseline_stats["total_depth"] * 100) if baseline_stats["total_depth"] > 0 else 0

    if spread_change < 0:
        lines.append(f"  Spread REDUCED by {abs(spread_change):.1f}% — HFT improves price efficiency.")
    elif spread_change > 0:
        lines.append(f"  Spread INCREASED by {spread_change:.1f}% — HFT did not tighten spreads.")
    else:
        lines.append("  Spread unchanged.")

    if depth_change > 0:
        lines.append(f"  Depth INCREASED by {depth_change:.1f}% — HFT adds liquidity.")
    elif depth_change < 0:
        lines.append(f"  Depth DECREASED by {abs(depth_change):.1f}%.")
    else:
        lines.append("  Depth unchanged.")

    lines.append("")
    lines.append("Note: Both runs use the same ITCH input file and deterministic")
    lines.append("processing, ensuring reproducible results.")
    lines.append("=" * 70)

    report = "\n".join(lines) + "\n"

    # Write to file and stdout
    report_path = os.path.join(results_dir, "market_impact_report.txt")
    with open(report_path, "w") as f:
        f.write(report)

    print(report)
    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
