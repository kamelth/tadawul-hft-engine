# Baseline v1 — Fixed-Spread Market Maker
**Date:** 2026-04-28  
**Data:** NASDAQ ITCH 5.0 — January 30, 2020 (5M messages)  
**Symbols:** AAPL, MSFT, GOOGL, AMZN, TSLA  
**Engine:** hft_strategy --strategy market_maker --max-messages 5000000

---

## Strategy Parameters (Fixed)

| Parameter        | Value        |
|------------------|-------------|
| Spread           | 10 ticks ($0.0010) — FIXED for all symbols |
| Quote size       | 100 shares  |
| Max position     | 1,000 shares |
| Inventory skew   | 1 tick/share |
| Volatility model | None        |
| Flow signal      | None        |

---

## Fill Summary

| Symbol | Buys | Sells | Final Position | Avg Buy   | Avg Sell  | Realized PnL |
|--------|------|-------|---------------|-----------|-----------|-------------|
| AAPL   | 119  | 119   | 931 sh        | $321.25   | $321.41   | **+$386.47** |
| AMZN   | 84   | 5     | 1,005 sh      | $1852.69  | $1848.66  | -$24.20     |
| GOOGL  | 99   | 45    | 928 sh        | $1440.44  | $1438.83  | -$932.58    |
| MSFT   | 40   | 1     | 1,088 sh      | $174.50   | $172.70   | -$180.18    |
| TSLA   | 142  | 239   | 0 sh          | $640.44   | $639.35   | **-$5,975.33** |
| **TOTAL** | **484** | **409** | | | | **-$6,725.83** |

**Total fills: 893**

---

## Performance Metrics

| Metric               | Value              |
|----------------------|--------------------|
| ITCH throughput      | ~14.1M msgs/sec    |
| Latency p50          | 1 ns               |
| Latency p95          | 64 ns              |
| Latency p99          | 256 ns             |
| Strategy decide p50  | 128 ns             |
| Strategy quotes      | 57,863 quotes sent |

---

## Problems Identified

### 1. Fixed spread hurts on volatile symbols
- TSLA moves $1–2 in minutes. A $0.001 spread earns $0.10/round-trip but risks $200 on a 1-share move.
- Fix: **volatility-adaptive spread** — `spread = k × σ`

### 2. Adverse selection on TSLA
- 142 buys vs 239 sells. Strategy bought into falling price repeatedly.
- Aggressive sellers knew price direction; strategy did not.
- Fix: **order flow imbalance signal** — lean quotes against incoming pressure.

### 3. Inventory accumulation (AMZN, MSFT, GOOGL)
- AMZN: 84 buys, only 5 sells → stuck long 1,005 shares
- Strategy couldn't sell because price fell below cost basis
- Same root cause as #1 and #2: no volatility/flow awareness

---

## Plots Generated

| File | What it shows |
|------|--------------|
| `00_dashboard.png`           | All symbols PnL over time + bar summary |
| `01_position_pnl_*.png`      | Per-symbol inventory + cumulative PnL (2 panels) |
| `02_fill_vs_mid_*.png`       | Fill price vs mid-price scatter |
| `03_pnl_bar_chart.png`       | Realized PnL by symbol |
| `04_fill_count.png`          | Buy vs sell fill count stacked bar |
| `05_fill_distribution.png`   | Fill price histogram per symbol |
| `06_spread_at_fill.png`      | Market spread at time of fill |

---

## Validation

- **compare_strategy.py:** VALIDATION PASSED — all 893 fills match Python recomputation bit-for-bit
- **validate_hftbacktest.py:** 4/5 symbols direction match (AAPL mismatch due to snapshot granularity)

---

## What Comes Next (v2)

**Planned improvements using GPU acceleration:**

1. **Volatility-adaptive spread** — GPU analytics kernel computes `σ` per symbol per update
   - Expected impact: reduce TSLA loss significantly, fewer fills at wrong price
   
2. **Order flow imbalance signal** — GPU kernel outputs `imbalance = (bid_vol - ask_vol) / total`
   - Expected impact: shift quotes before adverse price moves

**Hypothesis:** Combining both signals will:
- Reduce TSLA realized loss from -$5,975 toward breakeven
- Reduce overall inventory accumulation (AMZN, MSFT, GOOGL)
- Slightly reduce total fill count (wider spread = fewer but better fills)
- Improve total PnL from -$6,725 toward positive

**Measurement plan after v2:**
- Same 5M messages, same 5 symbols, same parameters except spread/skew now GPU-computed
- Compare: fill count, realized PnL per symbol, total PnL, inventory levels
- Generate same 7 plots for side-by-side comparison
