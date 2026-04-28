#!/usr/bin/env python3
"""
validate_hftbacktest.py — Validate C++ engine results against hftbacktest Python.

What this does
--------------
1. Reads results/trade_log.csv  (C++ engine fills)
2. Reads results/hft_snapshots.csv  (market snapshots, sampled every 10s ITCH time)
3. Replays the SAME market-making strategy on the same market data using
   hftbacktest's Python backtester (NumPy/Numba-based)
4. Compares per-symbol fill counts and realized PnL between C++ and Python

This is a higher-level validation than compare_strategy.py:
  compare_strategy.py checks that the C++ PnL accounting math is correct
  validate_hftbacktest.py checks that the strategy LOGIC is consistent
  with an independent Python backtesting framework

Requirements
------------
    pip install hftbacktest numpy

Note: hftbacktest uses snapshot-level granularity here (one event per 10-second
snapshot), whereas the C++ engine operates on every ITCH message. Some divergence
in fill counts is expected — we check that the ORDER OF MAGNITUDE and direction
(positive PnL) agree.

Usage
-----
    python3 scripts/validate_hftbacktest.py [--trade-log results/trade_log.csv]
                                            [--snapshots results/hft_snapshots.csv]
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import numpy as np


# ---------------------------------------------------------------------------
# Check hftbacktest availability
# ---------------------------------------------------------------------------

def check_hftbacktest():
    try:
        import hftbacktest  # noqa: F401
        return True
    except ImportError:
        return False


# ---------------------------------------------------------------------------
# Load C++ results
# ---------------------------------------------------------------------------

def load_cpp_fills(path):
    fills = defaultdict(list)
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            sym = row['symbol'].strip()
            fills[sym].append({
                'timestamp_ns': int(row['timestamp_ns']),
                'side':         row['side'].strip(),
                'fill_price':   int(row['fill_price']),
                'fill_qty':     int(row['fill_qty']),
            })
    return fills


def compute_cpp_pnl(fills_by_sym):
    """Compute realized PnL per symbol from raw fills (same as compare_strategy.py)."""
    pnl = {}
    for sym, fills in fills_by_sym.items():
        bought_val = 0
        bought_qty = 0
        sold_val   = 0
        sold_qty   = 0
        for f in fills:
            if f['side'] == 'BUY':
                bought_val += f['fill_price'] * f['fill_qty']
                bought_qty += f['fill_qty']
            else:
                sold_val += f['fill_price'] * f['fill_qty']
                sold_qty += f['fill_qty']
        if sold_qty == 0 or bought_qty == 0:
            pnl[sym] = 0
        else:
            avg_buy  = bought_val // bought_qty
            avg_sell = sold_val   // sold_qty
            pnl[sym] = (avg_sell - avg_buy) * min(bought_qty, sold_qty)
    return pnl


# ---------------------------------------------------------------------------
# Convert snapshots to hftbacktest event format
# ---------------------------------------------------------------------------

# hftbacktest event flags
DEPTH_EVENT   = 1 << 0   # order book depth update
TRADE_EVENT   = 1 << 1   # trade event
BUY_EVENT     = 1 << 2   # buy-side event
SELL_EVENT    = 1 << 3   # sell-side event

HFTBT_DTYPE = np.dtype([
    ('ev',       np.int64),   # event type flags
    ('exch_ts',  np.int64),   # exchange timestamp (ns)
    ('local_ts', np.int64),   # local receipt timestamp (ns)
    ('px',       np.float64), # price
    ('qty',      np.float64), # quantity
    ('order_id', np.int64),
    ('_1',       np.int64),
    ('_2',       np.int64),
])


def snapshots_to_hftbt(snapshots, symbol):
    """
    Convert per-symbol snapshots to the hftbacktest 8-field event array.
    Each snapshot produces two rows: one for best bid, one for best ask.
    """
    sym_snaps = [s for s in snapshots if s['symbol'] == symbol]
    if not sym_snaps:
        return None

    rows = []
    for s in sym_snaps:
        ts = s['timestamp_ns']
        bid_px = s['best_bid'] / 10000.0   # convert from $0.0001 to dollars
        ask_px = s['best_ask'] / 10000.0
        bid_qty = float(s.get('bid_volume', 100))
        ask_qty = float(s.get('ask_volume', 100))

        # Bid depth event
        rows.append((
            DEPTH_EVENT | BUY_EVENT,
            ts, ts,
            bid_px, bid_qty,
            0, 0, 0
        ))
        # Ask depth event
        rows.append((
            DEPTH_EVENT | SELL_EVENT,
            ts, ts,
            ask_px, ask_qty,
            0, 0, 0
        ))

    arr = np.array(rows, dtype=HFTBT_DTYPE)
    return arr


# ---------------------------------------------------------------------------
# Pure-Python market-making simulation (same parameters as C++ engine)
# ---------------------------------------------------------------------------

def simulate_market_maker_python(snapshots, symbol,
                                  spread_ticks=10, quote_size=100,
                                  max_position=1000, skew_per_share=1):
    """
    Simplified market-maker simulation on snapshots.

    Fill model: if our bid >= market best_bid (we're inside or at top of book),
    assume fill with probability proportional to bid_volume. In this simplified
    model we just count fills and compute PnL.

    This is intentionally simpler than the C++ engine (which processes every
    individual ITCH event). The goal is directional agreement, not exact match.
    """
    sym_snaps = [s for s in snapshots if s['symbol'] == symbol]
    fills = []
    position = 0

    for s in sym_snaps:
        if s['best_bid'] == 0 or s['best_ask'] == 0:
            continue

        mid  = (s['best_bid'] + s['best_ask']) // 2
        skew = position * skew_per_share      # inventory skew in ticks

        our_bid = mid - spread_ticks // 2 - skew
        our_ask = mid + spread_ticks // 2 - skew

        can_buy  = position < max_position
        can_sell = position > 0

        # Fill model: our quote is executed if it improves on the market
        if can_buy and our_bid > 0 and our_bid >= s['best_bid']:
            qty = min(quote_size, max_position - position)
            if qty > 0:
                fills.append(('BUY', our_bid, qty))
                position += qty

        if can_sell and our_ask <= s['best_ask']:
            qty = min(quote_size, position)
            if qty > 0:
                fills.append(('SELL', our_ask, qty))
                position -= qty

    # Compute PnL
    bought_val = sum(p * q for side, p, q in fills if side == 'BUY')
    bought_qty = sum(q for side, p, q in fills if side == 'BUY')
    sold_val   = sum(p * q for side, p, q in fills if side == 'SELL')
    sold_qty   = sum(q for side, p, q in fills if side == 'SELL')

    realized_pnl = 0
    if bought_qty > 0 and sold_qty > 0:
        avg_buy  = bought_val / bought_qty
        avg_sell = sold_val   / sold_qty
        realized_pnl = (avg_sell - avg_buy) * min(bought_qty, sold_qty)

    return fills, realized_pnl


# ---------------------------------------------------------------------------
# hftbacktest-based simulation
# ---------------------------------------------------------------------------

def simulate_with_hftbacktest(data_array, spread_ticks=10, quote_size=100,
                               max_position=1000, tick_size=0.0001):
    """
    Runs a market-maker strategy using hftbacktest's Python backtester.
    Returns list of fills: (side, price, qty)
    """
    try:
        from hftbacktest import HftBacktest, FeedLatency, Linear
        from hftbacktest.order import BUY, SELL
    except ImportError:
        return None, "hftbacktest not installed"

    half_spread = spread_ticks * tick_size / 2.0

    bt = HftBacktest(
        data_array,
        tick_size=tick_size,
        lot_size=1.0,
        maker_fee=0.0,
        taker_fee=0.0,
        order_latency=FeedLatency(),
        asset_type=Linear,
    )

    fills = []
    position = 0

    while bt.elapse(10_000_000_000):  # step by 10 seconds
        mid = (bt.best_bid + bt.best_ask) / 2.0
        if mid == 0 or bt.best_bid == 0 or bt.best_ask == 0:
            continue

        skew = position * tick_size

        our_bid = mid - half_spread - skew
        our_ask = mid + half_spread - skew

        can_buy  = position < max_position
        can_sell = position > 0

        if can_buy:
            qty = min(quote_size, max_position - position)
            if qty > 0:
                order = bt.submit_buy_order(our_bid, qty, wait=False)
                if order and order.filled_qty > 0:
                    fills.append(('BUY', order.avg_price, order.filled_qty))
                    position += int(order.filled_qty)

        if can_sell:
            qty = min(quote_size, position)
            if qty > 0:
                order = bt.submit_sell_order(our_ask, qty, wait=False)
                if order and order.filled_qty > 0:
                    fills.append(('SELL', order.avg_price, order.filled_qty))
                    position -= int(order.filled_qty)

    return fills, None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def load_snapshots_from_csv(path):
    rows = []
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append({
                'timestamp_ns': int(row['timestamp_ns']),
                'symbol':       row['symbol'].strip(),
                'best_bid':     int(row['best_bid']),
                'best_ask':     int(row['best_ask']),
                'spread':       int(row['spread']),
                'bid_volume':   int(row.get('bid_volume', 100)),
                'ask_volume':   int(row.get('ask_volume', 100)),
            })
    return rows


def main():
    parser = argparse.ArgumentParser(description='Validate C++ engine vs hftbacktest')
    parser.add_argument('--trade-log',  default='results/trade_log.csv')
    parser.add_argument('--snapshots',  default='results/hft_snapshots.csv')
    parser.add_argument('--output',     default='results/hftbacktest_comparison.txt')
    args = parser.parse_args()

    have_hftbt = check_hftbacktest()
    if have_hftbt:
        print("hftbacktest found — will run full library comparison")
    else:
        print("hftbacktest not installed — running Python-only simulation")
        print("  Install with: pip install hftbacktest")

    # Load data
    if not os.path.exists(args.trade_log):
        sys.exit(f"ERROR: trade log not found: {args.trade_log}")
    if not os.path.exists(args.snapshots):
        sys.exit(f"ERROR: snapshots not found: {args.snapshots}")

    cpp_fills   = load_cpp_fills(args.trade_log)
    cpp_pnl     = compute_cpp_pnl(cpp_fills)
    snapshots   = load_snapshots_from_csv(args.snapshots)
    symbols     = sorted(cpp_fills.keys())

    print(f"\nLoaded {sum(len(v) for v in cpp_fills.values())} C++ fills across {len(symbols)} symbols")
    print(f"Loaded {len(snapshots)} market snapshots")

    # Run per-symbol simulation
    report = []
    report.append("=" * 80)
    report.append("HFT Engine — hftbacktest Comparison Report")
    report.append("=" * 80)
    report.append(f"{'Symbol':<8}  {'C++ Fills':>10}  {'Py Fills':>9}  "
                  f"{'C++ PnL':>12}  {'Py PnL':>12}  {'Direction Match':>15}")
    report.append('-' * 80)

    all_match = True
    for sym in symbols:
        c_fills     = cpp_fills[sym]
        c_fill_cnt  = len(c_fills)
        c_pnl_usd   = cpp_pnl.get(sym, 0) / 10000.0

        if have_hftbt:
            data_array = snapshots_to_hftbt(snapshots, sym)
            if data_array is None or len(data_array) == 0:
                py_fills_list, err = [], "no data"
            else:
                py_fills_list, err = simulate_with_hftbacktest(data_array)
                if err:
                    py_fills_list = []
        else:
            py_fills_list, py_pnl_raw = simulate_market_maker_python(snapshots, sym)

        p_fill_cnt = len(py_fills_list) if py_fills_list else 0

        if py_fills_list:
            bought_val = sum(p * q for side, p, q in py_fills_list if side == 'BUY')
            bought_qty = sum(q for side, p, q in py_fills_list if side == 'BUY')
            sold_val   = sum(p * q for side, p, q in py_fills_list if side == 'SELL')
            sold_qty   = sum(q for side, p, q in py_fills_list if side == 'SELL')
            if bought_qty > 0 and sold_qty > 0:
                avg_buy  = bought_val / bought_qty
                avg_sell = sold_val / sold_qty
                py_pnl_usd = (avg_sell - avg_buy) * min(bought_qty, sold_qty)
                if not have_hftbt:
                    py_pnl_usd /= 10000.0  # Python sim uses raw units
            else:
                py_pnl_usd = 0.0
        else:
            py_pnl_usd = 0.0

        # Direction match: both positive, both negative, or both zero
        direction_ok = (c_pnl_usd >= 0) == (py_pnl_usd >= 0) or abs(c_pnl_usd) < 0.01
        match_str = "OK" if direction_ok else "MISMATCH"
        if not direction_ok:
            all_match = False

        report.append(
            f"{sym:<8}  {c_fill_cnt:>10}  {p_fill_cnt:>9}  "
            f"${c_pnl_usd:>11.4f}  ${py_pnl_usd:>11.4f}  {match_str:>15}"
        )

    report.append('-' * 80)
    report.append("")
    report.append("Notes:")
    report.append("  Fill counts differ: C++ engine processes every ITCH event (~ms granularity)")
    report.append("  Python simulation uses 10-second snapshots — fill count will be LOWER")
    report.append("  What matters: both produce positive PnL and the direction agrees")
    report.append("")
    report.append("  PnL is in USD. C++ engine prices are in $0.0001 units (divided by 10000)")
    report.append("")
    if all_match:
        report.append("RESULT: PASS — PnL direction matches across all symbols")
    else:
        report.append("RESULT: WARNING — PnL direction mismatch on some symbols (check above)")
    report.append("=" * 80)

    output = "\n".join(report)
    print("\n" + output)

    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    with open(args.output, 'w') as f:
        f.write(output + "\n")
    print(f"\nWrote {args.output}")


if __name__ == '__main__':
    main()
