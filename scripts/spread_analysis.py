"""
spread_analysis.py — Compute effective spread and realized spread from run outputs.

Inputs (from a strategy run):
  - trade_log.csv   : timestamp_ns, symbol, side, fill_price, mid_at_fill, effective_spread_bps, ...
  - hft_snapshots.csv : timestamp_ns, symbol, best_bid, best_ask, spread, relative_spread_bps, ...

Outputs:
  - Per-symbol table: quoted / relative / effective / realized spread
  - Printed to stdout + saved to spread_report.txt
"""

import sys
import os
import pandas as pd
import numpy as np

WINDOW_NS = 30 * 1_000_000_000  # 30-second forward window for realized spread

def load_data(run_dir):
    fills_path = os.path.join(run_dir, "trade_log.csv")
    snaps_path = os.path.join(run_dir, "hft_snapshots.csv")

    fills = pd.read_csv(fills_path)
    snaps = pd.read_csv(snaps_path)

    # Filter snapshots to only rows with valid bid and ask
    snaps = snaps[(snaps["best_bid"] > 0) & (snaps["best_ask"] > 0)].copy()
    snaps["mid"] = (snaps["best_bid"] + snaps["best_ask"]) / 2

    return fills, snaps


def realized_spread(fills, snaps):
    """
    For each fill, find the mid-price 30 seconds later (in ITCH time).
    realized_spread = 2 × (fill_price − mid_30s)  for SELL
    realized_spread = 2 × (mid_30s − fill_price)  for BUY
    Expressed in basis points relative to mid_at_fill.
    """
    records = []
    for _, fill in fills.iterrows():
        sym = fill["symbol"]
        ts  = fill["timestamp_ns"]
        mid_at_fill = fill.get("mid_at_fill", 0)
        if mid_at_fill == 0:
            continue

        sym_snaps = snaps[snaps["symbol"] == sym]
        future = sym_snaps[sym_snaps["timestamp_ns"] >= ts + WINDOW_NS]
        if future.empty:
            continue

        mid_later = future.iloc[0]["mid"]

        if fill["side"] == "SELL":
            rs = 2 * (fill["fill_price"] - mid_later)
        else:
            rs = 2 * (mid_later - fill["fill_price"])

        rs_bps = (rs * 10000.0 / mid_at_fill) if mid_at_fill > 0 else 0.0
        records.append({"symbol": sym, "side": fill["side"], "realized_spread_bps": rs_bps})

    return pd.DataFrame(records)


def run(run_dir):
    fills, snaps = load_data(run_dir)

    # ── Quoted spread (from snapshots) ──────────────────────────────────────
    quoted = (
        snaps.groupby("symbol")["spread"]
        .mean()
        .rename("avg_quoted_spread_ticks") / 10000.0  # convert to dollars
    )

    # ── Relative spread (from snapshots) ────────────────────────────────────
    if "relative_spread_bps" in snaps.columns:
        relative = snaps.groupby("symbol")["relative_spread_bps"].mean()
    else:
        # Compute on the fly if column not present (older run)
        snaps["rel_bps"] = snaps["spread"] * 10000.0 / snaps["mid"]
        relative = snaps.groupby("symbol")["rel_bps"].mean()
    relative.name = "avg_relative_spread_bps"

    # ── Effective spread (from fills) ────────────────────────────────────────
    if "effective_spread_bps" in fills.columns:
        effective = fills.groupby("symbol")["effective_spread_bps"].mean()
    else:
        # Compute on the fly if column not present (older run without mid_at_fill)
        fills_valid = fills[fills.get("mid_at_fill", pd.Series(dtype=float)) > 0].copy() \
            if "mid_at_fill" in fills.columns else pd.DataFrame()
        if not fills_valid.empty:
            fills_valid["eff_bps"] = (
                2.0 * (fills_valid["fill_price"] - fills_valid["mid_at_fill"]).abs()
                * 10000.0 / fills_valid["mid_at_fill"]
            )
            effective = fills_valid.groupby("symbol")["eff_bps"].mean()
        else:
            effective = pd.Series(dtype=float, name="effective_spread_bps")
    effective.name = "avg_effective_spread_bps"

    # ── Realized spread (30-second forward window) ──────────────────────────
    rs_df = realized_spread(fills, snaps)
    if not rs_df.empty:
        realized = rs_df.groupby("symbol")["realized_spread_bps"].mean()
    else:
        realized = pd.Series(dtype=float)
    realized.name = "avg_realized_spread_bps"

    # ── Combine ──────────────────────────────────────────────────────────────
    result = pd.concat([quoted, relative, effective, realized], axis=1)
    result = result.reset_index()

    # ── Fill count per symbol ────────────────────────────────────────────────
    fill_counts = fills.groupby("symbol").size().rename("fill_count")
    result = result.merge(fill_counts, on="symbol", how="left")

    return result


def print_report(result, run_dir):
    lines = []
    lines.append("=" * 72)
    lines.append("SPREAD MEASURES REPORT")
    lines.append(f"Run: {run_dir}")
    lines.append("=" * 72)
    lines.append("")
    lines.append(
        f"{'Symbol':<8} {'Quoted ($)':>12} {'Relative(bps)':>15} "
        f"{'Effective(bps)':>16} {'Realized(bps)':>15} {'Fills':>7}"
    )
    lines.append("-" * 72)

    for _, row in result.iterrows():
        def fmt(v, decimals=2):
            if pd.isna(v): return "N/A".rjust(decimals + 6)
            return f"{v:.{decimals}f}"

        lines.append(
            f"{row['symbol']:<8} "
            f"{fmt(row.get('avg_quoted_spread_ticks', float('nan')), 4):>12} "
            f"{fmt(row.get('avg_relative_spread_bps', float('nan')), 1):>15} "
            f"{fmt(row.get('avg_effective_spread_bps', float('nan')), 1):>16} "
            f"{fmt(row.get('avg_realized_spread_bps', float('nan')), 1):>15} "
            f"{int(row['fill_count']) if not pd.isna(row.get('fill_count', float('nan'))) else 'N/A':>7}"
        )

    lines.append("-" * 72)

    # Averages (excluding NaN)
    num_cols = ["avg_quoted_spread_ticks", "avg_relative_spread_bps",
                "avg_effective_spread_bps", "avg_realized_spread_bps"]
    avgs = result[num_cols].mean()
    lines.append(
        f"{'AVERAGE':<8} "
        f"{avgs.get('avg_quoted_spread_ticks', float('nan')):.4f}       "
        f"{avgs.get('avg_relative_spread_bps', float('nan')):.1f}            "
        f"{avgs.get('avg_effective_spread_bps', float('nan')):.1f}           "
        f"{avgs.get('avg_realized_spread_bps', float('nan')):.1f}"
    )
    lines.append("")
    lines.append("Notes:")
    lines.append("  Quoted ($)      = avg(best_ask - best_bid) over all snapshots")
    lines.append("  Relative (bps)  = avg(spread/mid × 10000) — cross-symbol comparison")
    lines.append("  Effective (bps) = avg(2×|fill_price - mid_at_fill|/mid × 10000) per fill")
    lines.append("  Realized (bps)  = avg(2×(fill_price - mid_30s_later)/mid × 10000) per fill")
    lines.append("                    Positive = market maker kept the spread (good)")
    lines.append("                    Negative = adverse selection (bad)")
    lines.append("=" * 72)

    report = "\n".join(lines)
    print(report)

    out_path = os.path.join(run_dir, "spread_report.txt")
    with open(out_path, "w") as f:
        f.write(report + "\n")
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    run_dir = sys.argv[1] if len(sys.argv) > 1 else "results/v2_adaptive"
    result = run(run_dir)
    print_report(result, run_dir)
