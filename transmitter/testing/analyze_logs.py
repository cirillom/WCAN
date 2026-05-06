#!/usr/bin/env python3
"""
WCAN Log Analyzer

Parses sensor and receiver logs from WCAN test runs, cross-references
sent vs received counter values per CAN ID, produces scatter plots
and text summaries.

Usage:
    uv run analyze_logs.py results/2026-04-22_174953/              # full run
    uv run analyze_logs.py results/2026-04-22_174953/test_2S-3R_rep0/  # single test

Dependencies:
    matplotlib (declared in pyproject.toml, installed via uv sync)
"""

import re
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt


# ── Data structures ──────────────────────────────────────────────────────────

@dataclass
class SensorData:
    board_id: str
    port: str
    can_id: str
    counters: list[int] = field(default_factory=list)
    counter_times: dict[int, float] = field(default_factory=dict)  # counter -> epoch seconds
    ack_delays: dict[int, float] = field(default_factory=dict)  # packet_tick -> delay in ms
    packet_counters: dict[int, int] = field(default_factory=dict)  # packet_tick -> first_counter in batch


@dataclass
class ReceiverData:
    board_id: str
    port: str
    received: dict[str, set[int]] = field(default_factory=dict)
    # can_id -> set of counter values
    received_times: dict[str, dict[int, float]] = field(default_factory=dict)
    # can_id -> counter -> epoch seconds


@dataclass
class DelayResult:
    sensor: SensorData
    receiver: ReceiverData
    delays: dict[int, float]  # counter -> delay in ms


@dataclass
class ComparisonResult:
    sensor: SensorData
    receiver: ReceiverData
    sent: list[int]
    received: set[int]
    missed: list[int]


@dataclass
class SensorSummary:
    """Per-sensor view: was each counter received by *any* receiver?"""
    sensor: SensorData
    sent: list[int]
    received_by_any: set[int]
    total_misses: list[int]


# ── Parsing ──────────────────────────────────────────────────────────────────

RE_SENSOR_CAN_ID = re.compile(r"SENSOR mode.*CAN ID:\s*0x([0-9a-fA-F]+)")
RE_SENSOR_CAN_PROC = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?CAN_PROC_\d+:\s*(0x[0-9a-fA-F]+)\s*batch\s*\d+\s*\[(\d+)\.\.\d+\]\s*at\s*\((\d+)\)"
)
RE_SENSOR_ACK = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?ACK:\s*Received ACK for packet tick\s*(\d+)"
)
RE_SENSOR_COUNTER = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?ReadDataTask:\s*(\d+)"
)
RE_RECEIVER_MODE = re.compile(r"RECEIVER mode")
RE_RECEIVER_BATCH = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?RecvCallback:\s*\[([0-9a-fA-F]+)\]\s*tick=(\d+)\s*\[(\d+)\.\.(\d+)\]\s*(\d+)\s*items"
)
RE_FILENAME = re.compile(r"(sensor|receiver)_([A-Za-z0-9]+)_(.+)\.log")


def _parse_ts(ts_str: str) -> float:
    return datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def parse_filename(path: Path) -> tuple[str, str, str]:
    """Extract (role, board_id, port) from filename like sensor_B0_COM21.log."""
    m = RE_FILENAME.match(path.name)
    if not m:
        raise ValueError(f"Unexpected filename format: {path.name}")
    return m.group(1), m.group(2), m.group(3)


def parse_sensor_log(path: Path) -> SensorData:
    """Parse a sensor log file and extract CAN ID + all sent counters."""
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    can_match = RE_SENSOR_CAN_ID.search(text)
    if not can_match:
        raise ValueError(f"No CAN ID found in sensor log: {path.name}")
    can_id = can_match.group(1).lower()

    counter_times: dict[int, float] = {}
    for m in RE_SENSOR_COUNTER.finditer(text):
        counter_times[int(m.group(2))] = _parse_ts(m.group(1))

    counters = sorted(counter_times.keys())
    
    can_proc_times = {}
    packet_counters = {}
    for m in RE_SENSOR_CAN_PROC.finditer(text):
        ts = _parse_ts(m.group(1))
        first_counter = int(m.group(3))
        tick = int(m.group(4))
        can_proc_times[tick] = ts
        packet_counters[tick] = first_counter
        
    ack_delays = {}
    for m in RE_SENSOR_ACK.finditer(text):
        ts = _parse_ts(m.group(1))
        tick = int(m.group(2))
        if tick in can_proc_times:
            ack_delays[tick] = (ts - can_proc_times[tick]) * 1000

    return SensorData(board_id=board_id, port=port, can_id=can_id, counters=counters, counter_times=counter_times, ack_delays=ack_delays, packet_counters=packet_counters)


def parse_receiver_log(path: Path) -> ReceiverData:
    """Parse a receiver log file and group received counters by CAN ID."""
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    if not RE_RECEIVER_MODE.search(text):
        raise ValueError(f"Not a receiver log: {path.name}")

    received: dict[str, set[int]] = {}
    received_times: dict[str, dict[int, float]] = {}
    for m in RE_RECEIVER_BATCH.finditer(text):
        ts = _parse_ts(m.group(1))
        can_id = m.group(2).lower()
        first, last = int(m.group(4)), int(m.group(5))
        received.setdefault(can_id, set()).update(range(first, last + 1))
        received_times.setdefault(can_id, {})[first] = ts

    return ReceiverData(board_id=board_id, port=port, received=received, received_times=received_times)


# ── Analysis ─────────────────────────────────────────────────────────────────

def compare(
    sensors: list[SensorData], receivers: list[ReceiverData]
) -> list[ComparisonResult]:
    results = []
    for sensor in sensors:
        sent = sensor.counters[:-1] if sensor.counters else []
        for receiver in receivers:
            recv_counters = receiver.received.get(sensor.can_id, set())

            # Trim trailing counters this receiver never saw
            missed_set = set(sent) - recv_counters
            n_tail = 0
            for c in reversed(sent):
                if c in missed_set:
                    n_tail += 1
                else:
                    break
            trimmed_sent = sent[:-n_tail] if n_tail else sent

            missed = sorted(c for c in trimmed_sent if c not in recv_counters)
            results.append(
                ComparisonResult(
                    sensor=sensor,
                    receiver=receiver,
                    sent=trimmed_sent,
                    received=recv_counters,
                    missed=missed,
                )
            )
    return results


def sensor_summary(
    sensors: list[SensorData], receivers: list[ReceiverData]
) -> list[SensorSummary]:
    """For each sensor, union all receivers to find total misses."""
    summaries = []
    for sensor in sensors:
        sent = sensor.counters[:-1] if sensor.counters else []
        union: set[int] = set()
        for receiver in receivers:
            union |= receiver.received.get(sensor.can_id, set())
        total_misses = sorted(c for c in sent if c not in union)

        # Trim any contiguous missed tail from sent — the monitoring cutoff
        # likely prevented receivers from logging those final packets.
        missed_set = set(total_misses)
        n_tail = 0
        for c in reversed(sent):
            if c in missed_set:
                n_tail += 1
            else:
                break
        if n_tail:
            tail_set = set(sent[-n_tail:])
            sent = sent[:-n_tail]
            total_misses = [c for c in total_misses if c not in tail_set]

        summaries.append(
            SensorSummary(
                sensor=sensor,
                sent=sent,
                received_by_any=union & set(sent),
                total_misses=total_misses,
            )
        )
    return summaries


# ── Text report ──────────────────────────────────────────────────────────────

def format_report(
    test_name: str,
    results: list[ComparisonResult],
    summaries: list[SensorSummary],
) -> str:
    """Generate a human-readable text summary."""
    lines = [f"=== {test_name} ==="]
    for r in results:
        n_sent = len(r.sent)
        n_recv = len(r.received)
        n_miss = len(r.missed)
        line = (
            f"Sensor {r.sensor.board_id} (0x{r.sensor.can_id}) → "
            f"Receiver {r.receiver.board_id} ({r.receiver.port}): "
            f"{n_sent} sent, {n_recv} received, {n_miss} missed"
        )
        if r.missed:
            line += f" {r.missed}"
        lines.append(line)

    lines.append("")
    lines.append("--- Sensor summary ---")
    for s in summaries:
        n_sent = len(s.sent)
        n_recv = len(s.received_by_any)
        n_miss = len(s.total_misses)
        line = (
            f"Sensor {s.sensor.board_id} (0x{s.sensor.can_id}): "
            f"{n_sent} sent, {n_recv} received by at least one receiver, "
            f"{n_miss} total misses"
        )
        if s.total_misses:
            line += f" {s.total_misses}"
        lines.append(line)

    lines.append("")
    return "\n".join(lines)


# ── Plotting ─────────────────────────────────────────────────────────────────

def plot_results(
    test_name: str,
    sensors: list[SensorData],
    receivers: list[ReceiverData],
    results: list[ComparisonResult],
    summaries: list[SensorSummary],
    output_path: Path,
) -> None:
    """Create a grid of subplots: rows=sensors, cols=receivers + ANY."""
    n_sensors = len(sensors)
    n_cols = len(receivers) + 1  # +1 for the ANY column

    fig, axes = plt.subplots(
        n_sensors,
        n_cols,
        figsize=(4 * n_cols + 1, 3 * n_sensors + 1),
        squeeze=False,
    )
    fig.suptitle(test_name, fontsize=14, fontweight="bold")

    # Build lookups
    comp_lookup = {
        (r.sensor.board_id, r.receiver.board_id): r for r in results
    }
    summary_lookup = {s.sensor.board_id: s for s in summaries}

    for row, sensor in enumerate(sensors):
        # Per-receiver columns
        for col, receiver in enumerate(receivers):
            ax = axes[row][col]
            comp = comp_lookup[(sensor.board_id, receiver.board_id)]
            _plot_subplot(
                ax,
                title=f"{sensor.board_id} (0x{sensor.can_id}) → {receiver.board_id}",
                sent=comp.sent,
                received_counters=set(comp.sent) - set(comp.missed),
                missed=comp.missed,
                n_recv_display=len(comp.received),
            )

        # ANY column (last)
        ax = axes[row][n_cols - 1]
        summary = summary_lookup[sensor.board_id]
        _plot_subplot(
            ax,
            title=f"{sensor.board_id} (0x{sensor.can_id}) → ANY",
            sent=summary.sent,
            received_counters=summary.received_by_any,
            missed=summary.total_misses,
            n_recv_display=len(summary.received_by_any),
            is_summary=True,
        )

    # Single legend for the whole figure
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=2, fontsize=9)

    plt.tight_layout(rect=[0, 0.04, 1, 0.95])
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)


def _plot_subplot(
    ax,
    title: str,
    sent: list[int],
    received_counters: set[int],
    missed: list[int],
    n_recv_display: int,
    is_summary: bool = False,
) -> None:
    """Render a single scatter subplot."""
    missed_set = set(missed)
    greens = [c for c in sent if c not in missed_set]
    reds = list(missed)

    ax.scatter(
        greens, [1] * len(greens),
        color="#2ecc71", s=50, zorder=3, label="Received",
    )
    ax.scatter(
        reds, [1] * len(reds),
        color="#e74c3c", s=50, zorder=4, marker="x", linewidths=2, label="Missed",
    )

    n_sent = len(sent)
    n_miss = len(missed)

    ax.set_title(title, fontsize=10, fontweight="bold", pad=12)

    color = "#e74c3c" if n_miss > 0 else "#2ecc71"
    ax.text(
        0.5, 1.01,
        f"{n_sent} sent, {n_recv_display} recv, {n_miss} missed",
        transform=ax.transAxes, ha="center", va="bottom",
        fontsize=8, color=color,
    )

    ax.set_xlabel("Counter")
    ax.set_yticks([])
    ax.set_ylim(0.5, 1.5)

    if sent:
        ax.set_xlim(min(sent) - 0.5, max(sent) + 0.5)

    ax.grid(axis="x", alpha=0.3)

    # Visual separator for the ANY column
    if is_summary:
        ax.spines["left"].set_linewidth(2.5)
        ax.spines["left"].set_color("#333333")


# ── Delay analysis ───────────────────────────────────────────────────────────

def plot_delays(
    test_name: str,
    sensors: list[SensorData],
    output_path: Path,
) -> None:
    """Scatter plot of per-packet send-to-ACK delay for each sensor."""
    sensors = sorted(sensors, key=lambda s: s.board_id)
    n_sensors = len(sensors)

    fig, axes = plt.subplots(
        n_sensors, 1,
        figsize=(8, 3 * n_sensors + 1),
        squeeze=False,
    )
    fig.suptitle(f"{test_name} — CAN_PROC to ACK Delay", fontsize=14, fontweight="bold")

    for row, sensor in enumerate(sensors):
        ax = axes[row][0]

        ax.set_title(
            f"{sensor.board_id} (0x{sensor.can_id})",
            fontsize=10, fontweight="bold",
        )
        ax.set_xlabel("First Counter in Batch")
        ax.set_ylabel("Delay (ms)")
        ax.grid(alpha=0.3)

        if not sensor.ack_delays:
            ax.text(0.5, 0.5, "No matched ACKs", transform=ax.transAxes, ha="center", va="center")
            continue

        ticks = sorted(sensor.ack_delays.keys())
        counters = [sensor.packet_counters[t] for t in ticks]
        delays_ms = [sensor.ack_delays[t] for t in ticks]
        median_d = sorted(delays_ms)[len(delays_ms) // 2]

        ax.scatter(counters, delays_ms, s=20, color="#3498db", alpha=0.7, zorder=3)
        ax.axhline(
            median_d, color="#e74c3c", linewidth=1.2, linestyle="--",
            label=f"median {median_d:.1f} ms",
        )
        ax.legend(fontsize=8)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ── Folder discovery ─────────────────────────────────────────────────────────

def is_test_folder(path: Path) -> bool:
    """Check if a folder contains sensor/receiver log files."""
    return any(path.glob("sensor_*.log")) and any(path.glob("receiver_*.log"))


def find_test_folders(root: Path) -> list[Path]:
    """Find all test subfolders under a results timestamp directory."""
    if is_test_folder(root):
        return [root]

    folders = sorted(p for p in root.iterdir() if p.is_dir() and is_test_folder(p))
    if not folders:
        print(f"No test folders found in {root}")
        sys.exit(1)
    return folders


# ── Single test analysis ─────────────────────────────────────────────────────

def analyze_test(test_dir: Path) -> str:
    """Analyze one test folder: parse, compare, plot, report."""
    test_name = test_dir.name

    # Parse all logs
    sensors = sorted(
        (parse_sensor_log(p) for p in test_dir.glob("sensor_*.log")),
        key=lambda s: s.board_id,
    )
    receivers = sorted(
        (parse_receiver_log(p) for p in test_dir.glob("receiver_*.log")),
        key=lambda r: r.board_id,
    )

    if not sensors:
        return f"=== {test_name} ===\nNo sensor logs found.\n"
    if not receivers:
        return f"=== {test_name} ===\nNo receiver logs found.\n"

    # Compare
    results = compare(sensors, receivers)
    summaries = sensor_summary(sensors, receivers)

    # Text report
    report = format_report(test_name, results, summaries)

    # Save text report
    report_path = test_dir / "analysis.txt"
    report_path.write_text(report, encoding="utf-8")

    # Plot packet delivery
    plot_path = test_dir / "analysis.png"
    plot_results(test_name, sensors, receivers, results, summaries, plot_path)

    # Plot delays
    delay_plot_path = test_dir / "analysis_delays.png"
    plot_delays(test_name, sensors, delay_plot_path)

    print(report)
    print(f"  Plot saved: {plot_path}")
    print(f"  Delay plot saved: {delay_plot_path}")
    print(f"  Report saved: {report_path}")
    print()

    return report


# ── Entry point ──────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: uv run analyze_logs.py <results_path>")
        print()
        print("  results_path can be:")
        print("    results/2026-04-22_174953/              (full run)")
        print("    results/2026-04-22_174953/test_2S-3R_rep0/  (single test)")
        sys.exit(1)

    root = Path(sys.argv[1])
    if not root.exists():
        print(f"Path not found: {root}")
        sys.exit(1)

    test_folders = find_test_folders(root)
    all_reports = []

    for folder in test_folders:
        report = analyze_test(folder)
        all_reports.append(report)

    # If multiple tests, write combined summary at root level
    if len(test_folders) > 1:
        summary_path = root / "analysis_summary.txt"
        summary_path.write_text("\n".join(all_reports), encoding="utf-8")
        print(f"Combined summary saved: {summary_path}")


if __name__ == "__main__":
    main()