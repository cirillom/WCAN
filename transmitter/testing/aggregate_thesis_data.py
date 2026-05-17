#!/usr/bin/env python3
"""
Aggregate WCAN measurement results across transport variants for the thesis
comparison table.

Usage:
    python aggregate_thesis_data.py results/v_bcast results/v_multicast \
        --output thesis_comparison.csv

Each input directory is expected to contain test folders matching
test_<S>S-<R>R_rep<n>/ produced by test_runner.py. The script parses each
folder's sensor_*.log and receiver_*.log via analysis.py and emits one CSV
row per (variant, test, repeat) with the headline measurement metrics.

The variant column is taken from the input directory's basename.

Empty cells indicate metrics whose underlying log lines were absent — usually
because the firmware was not built with -DMEASURE=1 or sdkconfig.measure.
"""
import argparse
import csv
import re
import sys
from pathlib import Path

from analysis import (
    parse_sensor_log,
    parse_receiver_log,
    sensor_summary,
    _percentile,
)

RE_TEST_FOLDER = re.compile(r"test_(\d+)S-(\d+)R_rep(\d+)")


def collect_test_folders(root: Path):
    for p in sorted(root.iterdir()):
        if not p.is_dir():
            continue
        m = RE_TEST_FOLDER.match(p.name)
        if not m:
            continue
        yield p, int(m.group(1)), int(m.group(2)), int(m.group(3))


def gather_metrics(test_folder: Path) -> dict:
    sensors = sorted(
        (parse_sensor_log(p) for p in test_folder.glob("sensor_*.log")),
        key=lambda s: s.board_id,
    )
    receivers = sorted(
        (parse_receiver_log(p) for p in test_folder.glob("receiver_*.log")),
        key=lambda r: r.board_id,
    )

    # Delivery rate: union of sender counters over all receivers
    summaries = sensor_summary(sensors, receivers)
    sent_total = sum(len(s.sent) for s in summaries)
    misses_total = sum(len(s.total_misses) for s in summaries)
    delivery_rate = (sent_total - misses_total) / sent_total if sent_total > 0 else None

    # Latency aggregations across sensors (unifying CB and RTT into us)
    all_lat_us = []
    lat_fail = 0
    for s in sensors:
        if s.measure.lat_cb_us:
            all_lat_us.extend(s.measure.lat_cb_us)
        elif s.measure.lat_rtt_ms:
            # Convert ms RTT to us
            all_lat_us.extend(int(ms * 1000) for ms in s.measure.lat_rtt_ms)
        lat_fail += s.measure.lat_cb_fail_count

    lat_sorted = sorted(all_lat_us)

    # Channel time percentage over the periodic measurement windows, averaged.
    util_samples = []
    for s in sensors:
        for (_, util, _, _) in s.measure.airtime_samples:
            util_samples.append(util)
    channel_time_pct_avg = (sum(util_samples) / len(util_samples) / 10.0) if util_samples else None

    # Cold-start: smallest receiver-side gap between BOOT_TS and FIRST_RX_TS
    cold_start_ms_per_recv = []
    for r in receivers:
        if r.measure.boot_ts_us >= 0 and r.measure.first_rx_ts_us >= 0:
            cold_start_ms_per_recv.append(
                (r.measure.first_rx_ts_us - r.measure.boot_ts_us) / 1000.0
            )
    cold_start_ms = min(cold_start_ms_per_recv) if cold_start_ms_per_recv else None

    # Stack HWM: minimum free bytes seen across all tasks on all devices
    min_hwm = None
    for d in sensors + receivers:
        for hwm in d.measure.task_hwm_min.values():
            if min_hwm is None or hwm < min_hwm:
                min_hwm = hwm

    def fmt_or_blank(value, fmt=None):
        if value is None:
            return ""
        return format(value, fmt) if fmt else value

    return {
        "active_sensor_count": len(sensors),
        "active_receiver_count": len(receivers),
        "total_packets_sent": sent_total,
        "total_packets_missed": misses_total,
        "delivery_success_rate": fmt_or_blank(delivery_rate, ".4f") if delivery_rate is not None else "",
        "latency_measurements_count": len(lat_sorted),
        "latency_median_microseconds": fmt_or_blank(_percentile(lat_sorted, 0.5)) if lat_sorted else "",
        "latency_99th_percentile_microseconds": fmt_or_blank(_percentile(lat_sorted, 0.99)) if lat_sorted else "",
        "latency_maximum_microseconds": lat_sorted[-1] if lat_sorted else "",
        "latency_failure_count": lat_fail,
        "channel_time_percent_average": fmt_or_blank(channel_time_pct_avg, ".2f") if channel_time_pct_avg is not None else "",
        "cold_start_minimum_milliseconds": fmt_or_blank(cold_start_ms, ".1f") if cold_start_ms is not None else "",
        "minimum_stack_high_water_mark_bytes": min_hwm if min_hwm is not None else "",
    }

def aggregate(variant_dirs, output_path) -> int:
    """Walk each directory in variant_dirs, parse test folders via analysis.py,
    write aggregated CSV to output_path. Returns the number of rows written.

    Returns 0 if no test data was found (output file is not created in that case).
    """
    output_path = Path(output_path)
    rows = []
    for variant_dir in variant_dirs:
        variant_dir = Path(variant_dir)
        if not variant_dir.is_dir():
            print(f"[WARN] Skipping non-directory: {variant_dir}", file=sys.stderr)
            continue
        variant = variant_dir.name
        for folder, s_count, r_count, rep in collect_test_folders(variant_dir):
            try:
                metrics = gather_metrics(folder)
            except Exception as exc:
                print(f"[WARN] Failed to parse {folder}: {exc}", file=sys.stderr)
                continue
            rows.append({
                "transport_variant": variant,
                "test_name": f"{s_count}S-{r_count}R",
                "configured_sensor_count": s_count,
                "configured_receiver_count": r_count,
                "test_repetition_index": rep,
                **metrics,
            })

    if not rows:
        return 0

    fieldnames = list(rows[0].keys())
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)
    return len(rows)


def main():
    parser = argparse.ArgumentParser(
        description="Aggregate WCAN thesis metrics across transport variants"
    )
    parser.add_argument(
        "variant_dirs", nargs="+", type=Path,
        help="Result directories, one per variant. Variant name is the dir basename.",
    )
    parser.add_argument(
        "--output", "-o", type=Path, default=Path("thesis_comparison.csv"),
        help="Output CSV path (default: thesis_comparison.csv)",
    )
    args = parser.parse_args()

    n = aggregate(args.variant_dirs, args.output)
    if n == 0:
        print("[ERROR] No test data found.", file=sys.stderr)
        sys.exit(1)

    print(f"Wrote {n} rows to {args.output}")


if __name__ == "__main__":
    main()
