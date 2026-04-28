#!/usr/bin/env python3
"""
compare_strategy.py — Cross-validate the C++ HFT engine's trade log.

Generates 6 plots:
  1. Position + Realized PnL over time  (per symbol, 2-panel)
  2. Fill price vs mid-price scatter    (per symbol)
  3. Per-symbol PnL bar chart           (summary)
  4. Buy vs Sell fill count             (stacked bar)
  5. Fill price distribution histogram  (per symbol)
  6. Market spread at fill time         (histogram)

Usage
-----
    python3 scripts/compare_strategy.py [--trade-log results/trade_log.csv]
                                        [--snapshots results/hft_snapshots.csv]
                                        [--output  results/validation_report.txt]
                                        [--plots   results/plots]
"""

import argparse
import csv
import os
import sys
from collections import defaultdict


# ---------------------------------------------------------------------------
# Pure-Python position tracker (mirrors C++ Position class exactly)
# ---------------------------------------------------------------------------

class Position:
    """Mirrors Trader::Strategy::Position.  All prices in $0.0001 integer units."""

    def __init__(self):
        self.shares         = 0
        self.total_bought   = 0
        self.total_sold     = 0
        self.avg_buy_price  = 0
        self.avg_sell_price = 0

    def buy(self, qty, price):
        if qty == 0:
            return
        if self.shares > 0:
            self.avg_buy_price = (self.shares * self.avg_buy_price + qty * price) \
                                 // (self.shares + qty)
        else:
            self.avg_buy_price = price
        self.shares       += qty
        self.total_bought += qty

    def sell(self, qty, price):
        if qty == 0:
            return
        if self.total_sold > 0:
            self.avg_sell_price = (self.total_sold * self.avg_sell_price + qty * price) \
                                  // (self.total_sold + qty)
        else:
            self.avg_sell_price = price
        self.shares    = max(0, self.shares - qty)
        self.total_sold += qty

    def realized_pnl(self):
        if self.total_sold == 0:
            return 0
        return (self.avg_sell_price - self.avg_buy_price) * self.total_sold

    def unrealized_pnl(self, current_price):
        if self.shares == 0:
            return 0
        return (current_price - self.avg_buy_price) * self.shares


# ---------------------------------------------------------------------------
# Load data
# ---------------------------------------------------------------------------

def load_trade_log(path):
    rows = []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            rows.append({
                'timestamp_ns':   int(row['timestamp_ns']),
                'symbol':         row['symbol'].strip(),
                'side':           row['side'].strip(),
                'fill_price':     int(row['fill_price']),
                'fill_qty':       int(row['fill_qty']),
                'position_after': int(row['position_after']),
                'realized_pnl':   int(row['realized_pnl']),
            })
    return rows


def load_snapshots(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            bid = int(row['best_bid'])
            ask = int(row['best_ask'])
            if bid > 0 and ask > 0:
                rows.append({
                    'timestamp_ns': int(row['timestamp_ns']),
                    'symbol':       row['symbol'].strip(),
                    'best_bid':     bid,
                    'best_ask':     ask,
                    'spread':       int(row['spread']),
                    'mid':          (bid + ask) // 2,
                })
    return rows


def nearest_snapshot(snap_by_sym, symbol, ts_ns):
    """Return the snapshot for symbol closest in time to ts_ns."""
    snaps = snap_by_sym.get(symbol, [])
    if not snaps:
        return None
    lo, hi = 0, len(snaps) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if snaps[mid]['timestamp_ns'] < ts_ns:
            lo = mid + 1
        else:
            hi = mid
    return snaps[lo]


# ---------------------------------------------------------------------------
# Independent PnL recomputation
# ---------------------------------------------------------------------------

def recompute_pnl(fills):
    positions = defaultdict(Position)
    errors    = []

    for i, f in enumerate(fills):
        sym = f['symbol']
        pos = positions[sym]

        if f['side'] == 'BUY':
            pos.buy(f['fill_qty'], f['fill_price'])
        elif f['side'] == 'SELL':
            pos.sell(f['fill_qty'], f['fill_price'])
        else:
            errors.append(f"Row {i}: unknown side '{f['side']}'")
            continue

        if pos.shares != f['position_after']:
            errors.append(
                f"Row {i} ({sym}): position_after mismatch — "
                f"C++={f['position_after']}, Python={pos.shares}"
            )
        py_rpnl = pos.realized_pnl()
        if py_rpnl != f['realized_pnl']:
            errors.append(
                f"Row {i} ({sym}): realized_pnl mismatch — "
                f"C++={f['realized_pnl']}, Python={py_rpnl}"
            )

    return positions, errors


# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------

def summarize(fills, positions):
    symbols      = sorted({f['symbol'] for f in fills})
    lines        = []
    total_realized = 0
    total_buys = total_sells = 0

    lines.append(f"{'Symbol':<8}  {'Buys':>6}  {'Sells':>6}  {'Shares':>8}  "
                 f"{'Avg Buy':>10}  {'Avg Sell':>10}  {'Realized PnL':>13}")
    lines.append('-' * 75)

    for sym in symbols:
        sf    = [f for f in fills if f['symbol'] == sym]
        buys  = sum(1 for f in sf if f['side'] == 'BUY')
        sells = sum(1 for f in sf if f['side'] == 'SELL')
        pos   = positions[sym]
        rpnl  = pos.realized_pnl()
        total_realized += rpnl
        total_buys  += buys
        total_sells += sells
        lines.append(
            f"{sym:<8}  {buys:>6}  {sells:>6}  {pos.shares:>8}  "
            f"${pos.avg_buy_price/10000:>9.4f}  "
            f"${pos.avg_sell_price/10000:>9.4f}  "
            f"${rpnl/10000:>12.4f}"
        )

    lines.append('-' * 75)
    lines.append(
        f"{'TOTAL':<8}  {total_buys:>6}  {total_sells:>6}  "
        f"{'':>8}  {'':>10}  {'':>10}  ${total_realized/10000:>12.4f}"
    )
    return lines, total_realized


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------

def make_plots(fills, positions, snapshots, output_dir):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        import matplotlib.ticker as mticker
        import numpy as np
    except ImportError:
        print("  matplotlib not installed — skipping plots")
        return

    os.makedirs(output_dir, exist_ok=True)
    symbols    = sorted({f['symbol'] for f in fills})
    COLORS     = ['#2196F3', '#4CAF50', '#FF9800', '#E91E63', '#9C27B0']
    sym_color  = {s: COLORS[i % len(COLORS)] for i, s in enumerate(symbols)}

    # Build snapshot lookup  {symbol: [sorted snaps]}
    snap_by_sym = defaultdict(list)
    for s in snapshots:
        snap_by_sym[s['symbol']].append(s)
    for v in snap_by_sym.values():
        v.sort(key=lambda x: x['timestamp_ns'])

    # ── Attach mid-price and spread to each fill ──────────────────────────────
    for f in fills:
        sn = nearest_snapshot(snap_by_sym, f['symbol'], f['timestamp_ns'])
        f['mid']    = sn['mid']    if sn else f['fill_price']
        f['spread'] = sn['spread'] if sn else 0

    # =========================================================================
    # PLOT 1 — Position + Realized PnL over time  (one file per symbol)
    # =========================================================================
    print("  [1/6] Position + PnL over time …")
    for sym in symbols:
        sf  = [f for f in fills if f['symbol'] == sym]
        ts  = [f['timestamp_ns'] / 1e9 for f in sf]
        pos = Position()
        pnls = []
        for f in sf:
            if f['side'] == 'BUY':
                pos.buy(f['fill_qty'], f['fill_price'])
            else:
                pos.sell(f['fill_qty'], f['fill_price'])
            pnls.append(pos.realized_pnl() / 10000)

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 6), sharex=True)
        c = sym_color[sym]

        ax1.fill_between(ts, [f['position_after'] for f in sf], alpha=0.25, color=c)
        ax1.plot(ts, [f['position_after'] for f in sf], color=c, linewidth=1.2)
        ax1.axhline(0, color='black', linewidth=0.7, linestyle='--')
        ax1.set_ylabel('Inventory (shares)', fontsize=10)
        ax1.set_title(f'{sym} — Market Maker: Inventory & Realized PnL', fontsize=12, fontweight='bold')
        ax1.grid(True, alpha=0.3)

        pos_pnl = [p for p in pnls if p >= 0]
        neg_pnl = [p for p in pnls if p < 0]
        color2  = '#27AE60' if not pnls or pnls[-1] >= 0 else '#E74C3C'
        ax2.plot(ts, pnls, color=color2, linewidth=1.2)
        ax2.fill_between(ts, pnls, 0, where=[p >= 0 for p in pnls],
                         alpha=0.2, color='#27AE60')
        ax2.fill_between(ts, pnls, 0, where=[p < 0 for p in pnls],
                         alpha=0.2, color='#E74C3C')
        ax2.axhline(0, color='black', linewidth=0.7, linestyle='--')
        ax2.set_ylabel('Realized PnL (USD)', fontsize=10)
        ax2.set_xlabel('Seconds since midnight (ITCH time)', fontsize=10)
        ax2.yaxis.set_major_formatter(mticker.FormatStrFormatter('$%.2f'))
        ax2.grid(True, alpha=0.3)

        plt.tight_layout()
        p = os.path.join(output_dir, f'01_position_pnl_{sym}.png')
        plt.savefig(p, dpi=150, bbox_inches='tight')
        plt.close()
        print(f"     {p}")

    # =========================================================================
    # PLOT 2 — Fill price vs mid-price scatter  (one file per symbol)
    # =========================================================================
    print("  [2/6] Fill price vs mid-price scatter …")
    for sym in symbols:
        sf   = [f for f in fills if f['symbol'] == sym]
        ts   = [f['timestamp_ns'] / 1e9 for f in sf]
        mids = [f['mid']        / 10000 for f in sf]
        fps  = [f['fill_price'] / 10000 for f in sf]
        buys  = [f for f in sf if f['side'] == 'BUY']
        sells = [f for f in sf if f['side'] == 'SELL']

        fig, ax = plt.subplots(figsize=(11, 5))
        ax.plot(ts, mids, color='#999999', linewidth=0.8, label='Mid price', zorder=1)
        ax.scatter([f['timestamp_ns']/1e9 for f in buys],
                   [f['fill_price']/10000 for f in buys],
                   color='#2196F3', marker='^', s=25, zorder=3, label='Buy fill')
        ax.scatter([f['timestamp_ns']/1e9 for f in sells],
                   [f['fill_price']/10000 for f in sells],
                   color='#E91E63', marker='v', s=25, zorder=3, label='Sell fill')
        ax.set_title(f'{sym} — Fill Price vs Mid-Price', fontsize=12, fontweight='bold')
        ax.set_ylabel('Price (USD)', fontsize=10)
        ax.set_xlabel('Seconds since midnight', fontsize=10)
        ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('$%.2f'))
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)

        plt.tight_layout()
        p = os.path.join(output_dir, f'02_fill_vs_mid_{sym}.png')
        plt.savefig(p, dpi=150, bbox_inches='tight')
        plt.close()
        print(f"     {p}")

    # =========================================================================
    # PLOT 3 — Per-symbol Realized PnL bar chart  (single summary chart)
    # =========================================================================
    print("  [3/6] Per-symbol PnL bar chart …")
    rpnls = []
    for sym in symbols:
        pos = positions[sym]
        rpnls.append(pos.realized_pnl() / 10000)

    colors_bar = ['#27AE60' if v >= 0 else '#E74C3C' for v in rpnls]

    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.bar(symbols, rpnls, color=colors_bar, edgecolor='white', linewidth=0.5)
    ax.axhline(0, color='black', linewidth=0.8)
    ax.set_title('Realized PnL by Symbol — Market Maker Strategy', fontsize=13, fontweight='bold')
    ax.set_ylabel('Realized PnL (USD)', fontsize=11)
    ax.set_xlabel('Symbol', fontsize=11)
    ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('$%.2f'))
    ax.grid(True, axis='y', alpha=0.3)
    for bar, v in zip(bars, rpnls):
        ax.text(bar.get_x() + bar.get_width()/2, v + (max(rpnls)-min(rpnls))*0.02,
                f'${v:.2f}', ha='center', va='bottom', fontsize=9, fontweight='bold')

    plt.tight_layout()
    p = os.path.join(output_dir, '03_pnl_bar_chart.png')
    plt.savefig(p, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"     {p}")

    # =========================================================================
    # PLOT 4 — Buy vs Sell fill count stacked bar
    # =========================================================================
    print("  [4/6] Buy vs Sell fill count …")
    buy_counts  = [sum(1 for f in fills if f['symbol'] == s and f['side'] == 'BUY')  for s in symbols]
    sell_counts = [sum(1 for f in fills if f['symbol'] == s and f['side'] == 'SELL') for s in symbols]

    x   = range(len(symbols))
    fig, ax = plt.subplots(figsize=(9, 5))
    ax.bar(x, buy_counts,  label='Buy fills',  color='#2196F3', alpha=0.85)
    ax.bar(x, sell_counts, bottom=buy_counts,  label='Sell fills', color='#E91E63', alpha=0.85)
    ax.set_xticks(list(x))
    ax.set_xticklabels(symbols)
    ax.set_title('Strategy Fill Count by Symbol', fontsize=13, fontweight='bold')
    ax.set_ylabel('Number of Fills', fontsize=11)
    ax.set_xlabel('Symbol', fontsize=11)
    ax.legend(fontsize=10)
    ax.grid(True, axis='y', alpha=0.3)
    for i, (b, s) in enumerate(zip(buy_counts, sell_counts)):
        ax.text(i, b + s + max(buy_counts+sell_counts)*0.01,
                str(b+s), ha='center', fontsize=9, fontweight='bold')

    plt.tight_layout()
    p = os.path.join(output_dir, '04_fill_count.png')
    plt.savefig(p, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"     {p}")

    # =========================================================================
    # PLOT 5 — Fill price distribution histogram  (all symbols, one figure)
    # =========================================================================
    print("  [5/6] Fill price distribution …")
    n_sym = len(symbols)
    cols  = min(3, n_sym)
    rows  = (n_sym + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(5*cols, 4*rows), squeeze=False)
    fig.suptitle('Fill Price Distribution by Symbol', fontsize=13, fontweight='bold')

    for idx, sym in enumerate(symbols):
        ax  = axes[idx // cols][idx % cols]
        sf  = [f for f in fills if f['symbol'] == sym]
        buy_prices  = [f['fill_price']/10000 for f in sf if f['side'] == 'BUY']
        sell_prices = [f['fill_price']/10000 for f in sf if f['side'] == 'SELL']
        all_prices  = [f['fill_price']/10000 for f in sf]
        if not all_prices:
            continue
        bins = min(30, max(5, len(all_prices)//5))
        ax.hist(buy_prices,  bins=bins, alpha=0.65, color='#2196F3', label='Buy')
        ax.hist(sell_prices, bins=bins, alpha=0.65, color='#E91E63', label='Sell')
        ax.set_title(sym, fontsize=11, fontweight='bold')
        ax.set_xlabel('Fill Price (USD)', fontsize=9)
        ax.set_ylabel('Count', fontsize=9)
        ax.xaxis.set_major_formatter(mticker.FormatStrFormatter('$%.1f'))
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    # hide unused subplots
    for idx in range(n_sym, rows*cols):
        axes[idx // cols][idx % cols].set_visible(False)

    plt.tight_layout()
    p = os.path.join(output_dir, '05_fill_distribution.png')
    plt.savefig(p, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"     {p}")

    # =========================================================================
    # PLOT 6 — Market spread at fill time histogram  (all symbols combined)
    # =========================================================================
    print("  [6/6] Market spread at fill time …")
    fig, ax = plt.subplots(figsize=(10, 5))
    for sym in symbols:
        sf      = [f for f in fills if f['symbol'] == sym]
        spreads = [f['spread']/10000 for f in sf if f['spread'] > 0]
        if not spreads:
            continue
        bins = min(40, max(5, len(spreads)//4))
        ax.hist(spreads, bins=bins, alpha=0.6, label=sym, color=sym_color[sym])

    ax.set_title('Market Spread at Fill Time', fontsize=13, fontweight='bold')
    ax.set_xlabel('Bid-Ask Spread (USD)', fontsize=11)
    ax.set_ylabel('Fill Count', fontsize=11)
    ax.xaxis.set_major_formatter(mticker.FormatStrFormatter('$%.4f'))
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    p = os.path.join(output_dir, '06_spread_at_fill.png')
    plt.savefig(p, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"     {p}")

    # =========================================================================
    # BONUS — Combined dashboard (all symbols, 2-panel, for thesis slide)
    # =========================================================================
    print("  [bonus] Combined PnL dashboard …")
    fig, (ax_top, ax_bot) = plt.subplots(2, 1, figsize=(14, 8), sharex=False)

    # Top: cumulative PnL curves for all symbols on one axis
    for sym in symbols:
        sf  = [f for f in fills if f['symbol'] == sym]
        ts  = [f['timestamp_ns']/1e9 for f in sf]
        pos = Position()
        pnls = []
        for f in sf:
            if f['side'] == 'BUY':
                pos.buy(f['fill_qty'], f['fill_price'])
            else:
                pos.sell(f['fill_qty'], f['fill_price'])
            pnls.append(pos.realized_pnl() / 10000)
        ax_top.plot(ts, pnls, label=sym, color=sym_color[sym], linewidth=1.4)

    ax_top.axhline(0, color='black', linewidth=0.8, linestyle='--')
    ax_top.set_title('Market Maker — Realized PnL Over Time (All Symbols)', fontsize=12, fontweight='bold')
    ax_top.set_ylabel('Realized PnL (USD)')
    ax_top.yaxis.set_major_formatter(mticker.FormatStrFormatter('$%.2f'))
    ax_top.legend(fontsize=9, ncol=len(symbols))
    ax_top.grid(True, alpha=0.3)

    # Bottom: per-symbol PnL bar chart
    rpnls2     = [positions[s].realized_pnl()/10000 for s in symbols]
    colors_b2  = ['#27AE60' if v >= 0 else '#E74C3C' for v in rpnls2]
    bars2      = ax_bot.bar(symbols, rpnls2, color=colors_b2, edgecolor='white')
    ax_bot.axhline(0, color='black', linewidth=0.8)
    ax_bot.set_title('Realized PnL Summary by Symbol', fontsize=12, fontweight='bold')
    ax_bot.set_ylabel('Realized PnL (USD)')
    ax_bot.yaxis.set_major_formatter(mticker.FormatStrFormatter('$%.2f'))
    ax_bot.grid(True, axis='y', alpha=0.3)
    for bar, v in zip(bars2, rpnls2):
        ax_bot.text(bar.get_x() + bar.get_width()/2,
                    v + (max(rpnls2) - min(rpnls2)) * 0.03,
                    f'${v:.2f}', ha='center', va='bottom', fontsize=9, fontweight='bold')

    plt.tight_layout()
    p = os.path.join(output_dir, '00_dashboard.png')
    plt.savefig(p, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"     {p}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description='Cross-validate C++ trade log + generate plots')
    parser.add_argument('--trade-log',  default='results/trade_log.csv')
    parser.add_argument('--snapshots',  default='results/hft_snapshots.csv')
    parser.add_argument('--output',     default='results/validation_report.txt')
    parser.add_argument('--plots',      default='results/plots')
    args = parser.parse_args()

    if not os.path.exists(args.trade_log):
        sys.exit(f"ERROR: trade log not found: {args.trade_log}\n"
                 "Run: ./hft_strategy data/... --strategy market_maker AAPL MSFT GOOGL")

    fills     = load_trade_log(args.trade_log)
    snapshots = load_snapshots(args.snapshots)
    print(f"Loaded {len(fills)} fills | {len(snapshots)} snapshots")

    if not fills:
        sys.exit("WARNING: trade log is empty — no strategy fills recorded.")

    positions, errors = recompute_pnl(fills)

    # Report
    lines = ["=" * 75,
             "HFT Engine — Python Cross-Validation Report",
             "=" * 75,
             f"Trade log:   {args.trade_log}",
             f"Total fills: {len(fills)}",
             ""]

    if errors:
        lines.append(f"VALIDATION ERRORS ({len(errors)} found):")
        for e in errors[:20]:
            lines.append(f"  {e}")
        if len(errors) > 20:
            lines.append(f"  ... and {len(errors)-20} more")
    else:
        lines.append("VALIDATION PASSED — Python PnL matches C++ on every fill")

    lines.append("")
    lines.append("Per-Symbol Summary (Python recomputed):")
    summary_lines, _ = summarize(fills, positions)
    lines.extend(summary_lines)
    lines += ["",
              "Price units: 1 unit = $0.0001  (integer prices, no float rounding)",
              "All PnL values are independently recomputed in Python.",
              "=" * 75]

    report = "\n".join(lines)
    print("\n" + report)
    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    with open(args.output, 'w') as f:
        f.write(report + "\n")
    print(f"\nWrote {args.output}")

    print("\nGenerating plots …")
    make_plots(fills, positions, snapshots, args.plots)
    print(f"\nAll plots saved to {args.plots}/")

    if errors:
        sys.exit(1)


if __name__ == '__main__':
    main()
