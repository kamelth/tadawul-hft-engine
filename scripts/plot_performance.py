#!/usr/bin/env python3
"""
plot_performance.py - Visualize Phase 5 performance artifacts.

Reads the CSVs emitted by hft_strategy into results/:
    latency_itch_message.csv
    latency_strategy_decide.csv
    throughput_itch_messages.csv
    throughput_book_events.csv
    throughput_strategy_quotes.csv

Produces PNG charts:
    results/latency_histogram.png
    results/throughput_timeseries.png

Usage:
    python scripts/plot_performance.py [results_dir]

Dependencies: matplotlib (pip install matplotlib).

Notes:
    The C++ engine emits these CSVs on every run and prints a text
    performance report directly. This script is ONLY for producing PNG
    charts for the thesis / defense slides. Skip it if you're happy with
    the text report.
"""

import csv
import os
import sys


def read_histogram(path):
    """Return (bucket_mids, counts) for a histogram CSV."""
    mids, counts = [], []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            low = float(row["bucket_low_ns"])
            high = float(row["bucket_high_ns"])
            mid = (low + high) / 2.0 if low > 0 else high / 2.0
            mids.append(mid)
            counts.append(int(row["count"]))
    return mids, counts


def read_throughput(path):
    """Return (elapsed, cumulative, rate) for a throughput CSV."""
    elapsed, cumulative, rate = [], [], []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            elapsed.append(float(row["elapsed_sec"]))
            cumulative.append(int(row["cumulative"]))
            rate.append(float(row["interval_rate"]))
    return elapsed, cumulative, rate


def plot_latency(results_dir):
    try:
        import matplotlib.pyplot as plt  # noqa: F401
    except ImportError:
        print("matplotlib not installed; skipping plots (pip install matplotlib)")
        return False
    import matplotlib.pyplot as plt

    histograms = {
        "ITCH message (total)": "latency_itch_message.csv",
        "Strategy decide":       "latency_strategy_decide.csv",
    }

    fig, axes = plt.subplots(1, len(histograms), figsize=(12, 5), sharey=True)
    if len(histograms) == 1:
        axes = [axes]

    plotted = False
    for ax, (label, fname) in zip(axes, histograms.items()):
        path = os.path.join(results_dir, fname)
        if not os.path.exists(path):
            ax.set_title(f"{label}\n(no data)")
            ax.set_xlabel("Latency (ns)")
            continue
        mids, counts = read_histogram(path)
        if not mids:
            ax.set_title(f"{label}\n(empty)")
            continue
        plotted = True
        ax.bar(range(len(mids)), counts, width=0.9)
        ax.set_xticks(range(len(mids)))
        ax.set_xticklabels([f"{int(m):,}" for m in mids], rotation=60, ha="right", fontsize=7)
        ax.set_xlabel("Latency bucket midpoint (ns)")
        ax.set_title(label)
        ax.grid(True, axis="y", alpha=0.3)

    axes[0].set_ylabel("Sample count")
    fig.suptitle("HFT Engine - Latency Distribution (wall-clock)", fontsize=13)
    fig.tight_layout()
    out = os.path.join(results_dir, "latency_histogram.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"Wrote {out}")
    return plotted


def plot_throughput(results_dir):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        return False

    sources = {
        "ITCH messages":   "throughput_itch_messages.csv",
        "Book events":     "throughput_book_events.csv",
        "Strategy quotes": "throughput_strategy_quotes.csv",
    }

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    plotted = False

    for label, fname in sources.items():
        path = os.path.join(results_dir, fname)
        if not os.path.exists(path):
            continue
        elapsed, cumulative, rate = read_throughput(path)
        if not elapsed:
            continue
        plotted = True
        ax1.plot(elapsed, rate, label=label, linewidth=1.5)
        ax2.plot(elapsed, cumulative, label=label, linewidth=1.5)

    ax1.set_ylabel("Interval rate (items / sec)")
    ax1.set_title("HFT Engine Throughput (interval rate)")
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="best")

    ax2.set_xlabel("Elapsed wall-clock (s)")
    ax2.set_ylabel("Cumulative count")
    ax2.set_title("Cumulative processed items")
    ax2.grid(True, alpha=0.3)
    ax2.legend(loc="best")

    fig.tight_layout()
    out = os.path.join(results_dir, "throughput_timeseries.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"Wrote {out}")
    return plotted


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "results"
    if not os.path.isdir(results_dir):
        print(f"Directory not found: {results_dir}", file=sys.stderr)
        sys.exit(1)

    any_data = False
    any_data |= bool(plot_latency(results_dir))
    any_data |= bool(plot_throughput(results_dir))

    if not any_data:
        print("No CSV artifacts found. Run hft_strategy first to produce them.")
        sys.exit(1)


if __name__ == "__main__":
    main()
