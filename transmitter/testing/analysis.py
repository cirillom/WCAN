#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path
import re
from dataclasses import dataclass, field
from datetime import datetime

# ── Data structures ──────────────────────────────────────────────────────────

@dataclass
class HeapStats:
    samples: list = field(default_factory=list)  # (ts, free, min_free, largest)
    slope_bytes_per_s: float = 0.0
    end_min_free: int = 0
    early_min_free: int = 0
    leak_status: str = "NO_DATA"

@dataclass
class MeasureStats:
    boot_ts_us: int = -1                # sensor or receiver
    first_rx_ts_us: int = -1            # receiver only
    airtime_samples: list = field(default_factory=list)   # (ts, util_per_mille, d_airtime_us, d_packets)
    lat_cb_us: list = field(default_factory=list)         # unicast: HW-ACK turnaround microseconds
    lat_cb_fail_count: int = 0
    lat_rtt_ms: list = field(default_factory=list)        # broadcast: app-ACK RTT milliseconds
    task_hwm_min: dict = field(default_factory=dict)      # name -> minimum hwm_bytes seen across samples

@dataclass
class SensorData:
    board_id: str
    port: str
    can_id: str
    counters: list = field(default_factory=list)
    counter_times: dict = field(default_factory=dict)
    ack_delays: dict = field(default_factory=dict)
    packet_counters: dict = field(default_factory=dict)
    sent_times: dict = field(default_factory=dict)
    crash_count: int = 0
    retry_count: int = 0
    max_retry_chain: int = 0
    heap: HeapStats = field(default_factory=HeapStats)
    measure: MeasureStats = field(default_factory=MeasureStats)

@dataclass
class ReceiverData:
    board_id: str
    port: str
    received: dict = field(default_factory=dict)
    received_times: dict = field(default_factory=dict)
    crash_count: int = 0
    heap: HeapStats = field(default_factory=HeapStats)
    measure: MeasureStats = field(default_factory=MeasureStats)

@dataclass
class DelayResult:
    sensor: SensorData
    receiver: ReceiverData
    delays: dict

@dataclass
class ComparisonResult:
    sensor: SensorData
    receiver: ReceiverData
    sent: list
    received: set
    missed: list

@dataclass
class SensorSummary:
    sensor: SensorData
    sent: list
    received_by_any: set
    total_misses: list

# ── Parsing ──────────────────────────────────────────────────────────────────

RE_SENSOR_CAN_ID = re.compile(r"SENSOR mode.*CAN ID:\s*0x([0-9a-fA-F]+)")
RE_SENSOR_CAN_PROC = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?CAN_PROC_\d+:\s*(0x[0-9a-fA-F]+)\s*batch\s*\d+\s*\[(\d+)\.\.\d+\]\s*at\s*\((\d+)\)"
)
RE_SENSOR_ACK = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?ACK:\s*Received ACK for packet tick\s*(\d+)"
)
RE_SENSOR_COUNTER = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?read_data_task:\s*(\d+)"
)
RE_RECEIVER_MODE = re.compile(r"RECEIVER mode")
RE_RECEIVER_BATCH = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?wcan_recv_callback:\s*\[([0-9a-fA-F]+)\]\s*tick=(\d+)\s*\[(\d+)\.\.(\d+)\]\s*(\d+)\s*items"
)
RE_FILENAME = re.compile(r"(sensor|receiver)_([A-Za-z0-9]+)_(.+)\.log")
RE_HEAP = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?HEAP:\s*free=(\d+)\s*min_free=(\d+)\s*largest=(\d+)"
)

# Phase 4 MEASURE instrumentation log lines
RE_BOOT_TS = re.compile(r"BOOT_TS:\s*us=(\d+)")
RE_FIRST_RX_TS = re.compile(r"FIRST_RX_TS:\s*us=(\d+)\s*id=0x([0-9a-fA-F]+)")
RE_MEASURE = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?MEASURE:\s*"
    r"airtime_us_total=(\d+)\s*packets_total=(\d+)\s*"
    r"d_airtime_us=(\d+)\s*d_packets=(\d+)\s*util_per_mille=(\d+)"
)
RE_LAT_CB = re.compile(
    r"LAT_CB:\s*peer=([0-9a-fA-F:]+)\s*dt_us=(-?\d+)\s*status=(\w+)"
)
RE_LAT_RTT = re.compile(
    r"LAT_RTT:\s*id=0x([0-9a-fA-F]+)\s*rtt_ms=(\d+)"
)
RE_TASK_STATS = re.compile(
    r"TASK:\s*name=(\S+)\s*prio=\d+\s*state=\d+\s*hwm_bytes=(\d+)\s*runtime=\d+\s*total_runtime=\d+"
)

CRASH_MARKER = "WCAN initialized"
RETRY_MARKER = "Timeout reached"
RE_RETRY_ATTEMPT = re.compile(r"Timeout reached.*?Attempt:\s*(\d+)\s*of\s*\d+")

LEAK_SLOPE_FAIL = -64.0
LEAK_SLOPE_SUSPECTED = -16.0
LEAK_END_DROP_RATIO = 0.80
HEAP_INIT_TRANSIENT_SEC = 5.0

def _parse_ts(ts_str: str) -> float:
    return datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S.%f").timestamp()

def parse_heap_stats(text: str) -> HeapStats:
    samples = []
    for m in RE_HEAP.finditer(text):
        ts = _parse_ts(m.group(1))
        samples.append((ts, int(m.group(2)), int(m.group(3)), int(m.group(4))))

    stats = HeapStats(samples=samples)
    if len(samples) < 3:
        return stats

    t0 = samples[0][0]
    post_init = [(t - t0, mf) for (t, _free, mf, _largest) in samples if (t - t0) >= HEAP_INIT_TRANSIENT_SEC]
    if len(post_init) < 3:
        post_init = [(t - t0, mf) for (t, _free, mf, _largest) in samples]

    n = len(post_init)
    sum_t = sum(t for t, _ in post_init)
    sum_y = sum(y for _, y in post_init)
    sum_tt = sum(t * t for t, _ in post_init)
    sum_ty = sum(t * y for t, y in post_init)
    denom = n * sum_tt - sum_t * sum_t
    slope = (n * sum_ty - sum_t * sum_y) / denom if denom != 0 else 0.0

    early_min_free = post_init[0][1]
    end_min_free = post_init[-1][1]
    stats.slope_bytes_per_s = slope
    stats.early_min_free = early_min_free
    stats.end_min_free = end_min_free

    if slope <= LEAK_SLOPE_FAIL or end_min_free < early_min_free * LEAK_END_DROP_RATIO:
        stats.leak_status = "FAIL"
    elif slope <= LEAK_SLOPE_SUSPECTED:
        stats.leak_status = "SUSPECTED"
    else:
        stats.leak_status = "OK"
    return stats

def parse_filename(path: Path) -> tuple:
    m = RE_FILENAME.match(path.name)
    if not m:
        raise ValueError(f"Unexpected filename format: {path.name}")
    return m.group(1), m.group(2), m.group(3)

def parse_measure(text: str) -> MeasureStats:
    """Parse Phase-4 MEASURE_INSTR log lines. Returns empty stats if MEASURE
    instrumentation wasn't compiled into the firmware (no matching lines).
    """
    stats = MeasureStats()

    m = RE_BOOT_TS.search(text)
    if m:
        stats.boot_ts_us = int(m.group(1))

    m = RE_FIRST_RX_TS.search(text)
    if m:
        stats.first_rx_ts_us = int(m.group(1))

    for m in RE_MEASURE.finditer(text):
        ts = _parse_ts(m.group(1))
        d_airtime_us = int(m.group(4))
        d_packets = int(m.group(5))
        util_per_mille = int(m.group(6))
        stats.airtime_samples.append((ts, util_per_mille, d_airtime_us, d_packets))

    for m in RE_LAT_CB.finditer(text):
        dt = int(m.group(2))
        status = m.group(3)
        if status == "OK" and dt >= 0:
            stats.lat_cb_us.append(dt)
        else:
            stats.lat_cb_fail_count += 1

    for m in RE_LAT_RTT.finditer(text):
        stats.lat_rtt_ms.append(int(m.group(2)))

    for m in RE_TASK_STATS.finditer(text):
        name = m.group(1)
        hwm = int(m.group(2))
        prev = stats.task_hwm_min.get(name)
        if prev is None or hwm < prev:
            stats.task_hwm_min[name] = hwm

    return stats

def parse_sensor_log(path: Path) -> SensorData:
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    can_match = RE_SENSOR_CAN_ID.search(text)
    if not can_match:
        raise ValueError(f"No CAN ID found in sensor log: {path.name}")
    can_id = can_match.group(1).lower()

    counter_times = {}
    for m in RE_SENSOR_COUNTER.finditer(text):
        counter_times[int(m.group(2))] = _parse_ts(m.group(1))
    counters = sorted(counter_times.keys())

    can_proc_times = {}
    packet_counters = {}
    sent_times = {}
    for m in RE_SENSOR_CAN_PROC.finditer(text):
        ts = _parse_ts(m.group(1))
        first_counter = int(m.group(3))
        tick = int(m.group(4))
        can_proc_times[tick] = ts
        packet_counters[tick] = first_counter
        sent_times[first_counter] = ts

    ack_delays = {}
    for m in RE_SENSOR_ACK.finditer(text):
        ts = _parse_ts(m.group(1))
        tick = int(m.group(2))
        if tick in can_proc_times:
            ack_delays[tick] = (ts - can_proc_times[tick]) * 1000

    crash_count = max(0, text.count(CRASH_MARKER) - 1)
    retry_count = text.count(RETRY_MARKER)
    max_retry_chain = max((int(m.group(1)) for m in RE_RETRY_ATTEMPT.finditer(text)), default=0)

    return SensorData(
        board_id=board_id,
        port=port,
        can_id=can_id,
        counters=counters,
        counter_times=counter_times,
        ack_delays=ack_delays,
        packet_counters=packet_counters,
        sent_times=sent_times,
        crash_count=crash_count,
        retry_count=retry_count,
        max_retry_chain=max_retry_chain,
        heap=parse_heap_stats(text),
        measure=parse_measure(text),
    )

def parse_receiver_log(path: Path) -> ReceiverData:
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    if not RE_RECEIVER_MODE.search(text):
        raise ValueError(f"Not a receiver log: {path.name}")

    received = {}
    received_times = {}
    for m in RE_RECEIVER_BATCH.finditer(text):
        ts = _parse_ts(m.group(1))
        can_id = m.group(2).lower()
        first, last = int(m.group(4)), int(m.group(5))
        received.setdefault(can_id, set()).update(range(first, last + 1))
        received_times.setdefault(can_id, {})[first] = ts

    crash_count = max(0, text.count(CRASH_MARKER) - 1)

    return ReceiverData(
        board_id=board_id,
        port=port,
        received=received,
        received_times=received_times,
        crash_count=crash_count,
        heap=parse_heap_stats(text),
        measure=parse_measure(text),
    )

def compare(sensors, receivers):
    results = []
    for sensor in sensors:
        sent = sensor.counters[:-1] if sensor.counters else []
        for receiver in receivers:
            recv_counters = receiver.received.get(sensor.can_id, set())

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

def compute_delays(sensors, receivers):
    results = []
    for sensor in sensors:
        for receiver in receivers:
            recv_times = receiver.received_times.get(sensor.can_id, {})
            delays = {}
            for first_counter, recv_ts in recv_times.items():
                send_ts = sensor.sent_times.get(first_counter)
                if send_ts is not None:
                    delays[first_counter] = (recv_ts - send_ts) * 1000
            results.append(DelayResult(sensor=sensor, receiver=receiver, delays=delays))
    return results

def is_test_folder(path: Path) -> bool:
    return any(path.glob("sensor_*.log")) and any(path.glob("receiver_*.log"))

def find_test_folders(root: Path) -> list:
    if is_test_folder(root):
        return [root]

    folders = sorted(p for p in root.iterdir() if p.is_dir() and is_test_folder(p))
    if not folders:
        print(f"No test folders found in {root}")
        sys.exit(1)
    return folders

#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path

def sensor_summary(sensors, receivers):
    summaries = []
    for sensor in sensors:
        sent = sensor.counters[:-1] if sensor.counters else []
        union = set()
        for receiver in receivers:
            union |= receiver.received.get(sensor.can_id, set())
        total_misses = sorted(c for c in sent if c not in union)

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

def analyze_data_overall(sensors, receivers, test_name):
    summaries = sensor_summary(sensors, receivers)
    report_lines = []
    total_misses = sum(len(s.total_misses) for s in summaries)

    status = "PASS" if total_misses == 0 else "FAIL"
    report_lines.append(f"=== {test_name} ===")
    
    report_lines.append(f"DATA SENT ANALYSIS: {status} ({total_misses} total misses)")

    for receiver in receivers:
        total_expected = sum(len(s.sent) for s in summaries)
        total_received = sum(len(receiver.received.get(s.sensor.can_id, set())) for s in summaries)
        total_missed = total_expected - total_received
        report_lines.append(f"Receiver {receiver.board_id} ({receiver.port}): {total_received} received (expected {total_expected}, missed {total_missed})")

    for s in summaries:
        sensor = s.sensor
        n_sent = len(s.sent)
        n_recv = len(s.received_by_any)
        n_miss = len(s.total_misses)
        retries_str = f"{sensor.retry_count}({sensor.max_retry_chain})" if sensor.max_retry_chain > 1 else f"{sensor.retry_count}"
        
        line = f"Sensor {sensor.board_id} (0x{sensor.can_id} | {sensor.port}): {n_sent} sent, {n_recv} received, {n_miss} misses, {retries_str} retries"
        report_lines.append(line)
        if s.total_misses:
            report_lines.append(f" - MISSED: {s.total_misses}")

    return "\n".join(report_lines), total_misses == 0

def analyze_data_pairs(sensors, receivers, test_name):
    results = compare(sensors, receivers)
    report_lines = []
    
    total_misses = 0
    report_lines.append(f"=== {test_name} PAIRS ===")
    for r in results:
        n_sent = len(r.sent)
        n_recv = len(r.received)
        total_misses += len(r.missed)
        line = f"Sensor {r.sensor.board_id} → Receiver {r.receiver.board_id}: {n_sent} sent, {n_recv} received"
        report_lines.append(line)
        if r.missed:
            report_lines.append(f" - MISSED: {r.missed}")
            
    passed = total_misses == 0
    return "\n".join(report_lines), passed

def analyze_crashes(sensors, receivers):
    """Analyze if there were crashes."""
    report_lines = []
    total_crashes = 0
    
    for device in sensors + receivers:
        if device.crash_count > 0:
            total_crashes += device.crash_count
            report_lines.append(f"{device.__class__.__name__} {device.board_id} crashed {device.crash_count} times")
            
    passed = total_crashes == 0
    status = "PASS" if passed else "FAIL"
    report_lines.insert(0, f"CRASH ANALYSIS: {status} ({total_crashes} total crashes)")
    return "\n".join(report_lines), passed

def _format_heap(stats) -> str:
    if stats.leak_status == "NO_DATA":
        return "no_samples"
    return (
        f"{stats.leak_status} (slope={stats.slope_bytes_per_s:+.1f} B/s, "
        f"min_free {stats.early_min_free}->{stats.end_min_free})"
    )

def analyze_heap_integrity(sensors, receivers):
    """Analyze heap integrity for leaks."""
    report_lines = []
    leak_fails = 0
    leak_suspect = 0
    
    for device in sensors + receivers:
        report_lines.append(f"{device.__class__.__name__} {device.board_id} heap: {_format_heap(device.heap)}")
        if device.heap.leak_status == "FAIL":
            leak_fails += 1
        elif device.heap.leak_status == "SUSPECTED":
            leak_suspect += 1
            
    passed = leak_fails == 0
    status = "PASS" if passed else "FAIL"
    report_lines.insert(0, f"HEAP INTEGRITY: {status} (Fails: {leak_fails}, Suspected: {leak_suspect})")
    return "\n".join(report_lines), passed

def _percentile(sorted_values: list, fraction: float):
    """fraction in [0, 1]. Linear nearest-rank percentile on a sorted list."""
    if not sorted_values:
        return None
    if fraction <= 0:
        return sorted_values[0]
    if fraction >= 1:
        return sorted_values[-1]
    idx = int(round(fraction * (len(sorted_values) - 1)))
    return sorted_values[idx]

def analyze_measure(sensors, receivers):
    """Summarize Phase-4 MEASURE_INSTR data: latency, airtime, stack HWM, cold-start.
    No-op when measurement instrumentation was not compiled in.
    """
    report_lines = []

    has_any = any(
        d.measure.lat_cb_us or d.measure.lat_rtt_ms or d.measure.airtime_samples
        or d.measure.boot_ts_us >= 0 or d.measure.task_hwm_min
        for d in sensors + receivers
    )
    if not has_any:
        return "MEASURE: no instrumentation data (build with -DMEASURE=1)", True

    report_lines.append("MEASURE:")

    for s in sensors:
        m = s.measure
        if m.lat_cb_us:
            xs = sorted(m.lat_cb_us)
            report_lines.append(
                f"  Sensor {s.board_id} HW-ACK latency (unicast): "
                f"n={len(xs)} median={_percentile(xs, 0.5)}us p99={_percentile(xs, 0.99)}us "
                f"max={xs[-1]}us fails={m.lat_cb_fail_count}"
            )
        if m.lat_rtt_ms:
            xs = sorted(m.lat_rtt_ms)
            report_lines.append(
                f"  Sensor {s.board_id} app-ACK RTT (broadcast): "
                f"n={len(xs)} median={_percentile(xs, 0.5)}ms p99={_percentile(xs, 0.99)}ms "
                f"max={xs[-1]}ms"
            )
        if m.airtime_samples:
            utils = [u for (_, u, _, _) in m.airtime_samples]
            avg_util = sum(utils) / len(utils)
            d_packets_total = sum(p for (_, _, _, p) in m.airtime_samples)
            d_airtime_total = sum(a for (_, _, a, _) in m.airtime_samples)
            report_lines.append(
                f"  Sensor {s.board_id} airtime: avg_util={avg_util:.1f}/1000 "
                f"d_packets_sum={d_packets_total} d_airtime_sum={d_airtime_total}us"
            )

    for r in receivers:
        m = r.measure
        if m.boot_ts_us >= 0 and m.first_rx_ts_us >= 0:
            cold_ms = (m.first_rx_ts_us - m.boot_ts_us) / 1000.0
            report_lines.append(
                f"  Receiver {r.board_id} cold-start to first frame: {cold_ms:.1f}ms"
            )

    # Aggregate stack HWM across all devices: report the minimum hwm seen per task
    # name (right-sizing signal: how close to overflow each task ran).
    all_task_hwm = {}
    for d in sensors + receivers:
        for name, hwm in d.measure.task_hwm_min.items():
            prev = all_task_hwm.get(name)
            if prev is None or hwm < prev:
                all_task_hwm[name] = hwm
    if all_task_hwm:
        report_lines.append("  Stack HWM (min free across all devices, smaller = closer to overflow):")
        for name in sorted(all_task_hwm):
            report_lines.append(f"    {name}: {all_task_hwm[name]} bytes free")

    return "\n".join(report_lines), True

def analyze_delay(sensors, receivers):
    """Analyze package delay between sensor and receivers."""
    delay_results = compute_delays(sensors, receivers)
    report_lines = []
    
    for d in delay_results:
        if not d.delays:
             report_lines.append(f"{d.sensor.board_id} → {d.receiver.board_id}: No matching batches")
             continue
        delays_ms = sorted(d.delays.values())
        median = delays_ms[len(delays_ms) // 2]
        max_d = delays_ms[-1]
        report_lines.append(f"{d.sensor.board_id} → {d.receiver.board_id}: median {median:.1f}ms, max {max_d:.1f}ms")
        
    report_lines.insert(0, f"DELAY ANALYSIS:")
    return "\n".join(report_lines), True

def analyze_all(test_dir: Path):
    """Run all analysis functions on a test directory."""
    test_name = test_dir.name

    sensors = sorted(
        (parse_sensor_log(p) for p in test_dir.glob("sensor_*.log")),
        key=lambda s: s.board_id,
    )
    receivers = sorted(
        (parse_receiver_log(p) for p in test_dir.glob("receiver_*.log")),
        key=lambda r: r.board_id,
    )

    if not sensors or not receivers:
         return f"No logs found in {test_dir}\n", False

    out = []
    
    r_data_o, p_data_o = analyze_data_overall(sensors, receivers, test_name)
    r_data_p, p_data_p = analyze_data_pairs(sensors, receivers, test_name)
    r_crash, p_crash = analyze_crashes(sensors, receivers)
    r_heap, p_heap = analyze_heap_integrity(sensors, receivers)
    r_delay, _ = analyze_delay(sensors, receivers)
    r_measure, _ = analyze_measure(sensors, receivers)

    out.append(r_data_o)
    out.append("")
    out.append(r_data_p)
    out.append("")
    out.append(r_crash)
    out.append("")
    out.append(r_heap)
    out.append("")
    out.append(r_delay)
    out.append("")
    out.append(r_measure)
    
    overall_passed = p_data_o and p_data_p and p_crash and p_heap
    out.insert(0, f"OVERALL STATUS: {'PASS' if overall_passed else 'FAIL'}\n")
    return "\n".join(out), overall_passed

def main():
    parser = argparse.ArgumentParser(description="WCAN Log Analyzer")
    parser.add_argument("results_path", help="Path to results directory")
    args = parser.parse_args()

    root = Path(args.results_path)
    if not root.exists():
        print(f"Path not found: {root}")
        sys.exit(1)

    test_folders = find_test_folders(root)
    n_passed = 0
    all_reports = []

    for folder in test_folders:
        report, passed = analyze_all(folder)
        all_reports.append(report)
        print(report)
        print("-" * 60)
        if passed:
            n_passed += 1

    result_line = f"RESULT: {n_passed}/{len(test_folders)} tests passed"
    print(result_line)

    if len(test_folders) > 1:
        summary_path = root / "analysis_summary.txt"
        combined = "\n".join(all_reports) + "\n" + result_line + "\n"
        summary_path.write_text(combined, encoding="utf-8")
        print(f"Combined summary saved: {summary_path}")

if __name__ == "__main__":
    main()
