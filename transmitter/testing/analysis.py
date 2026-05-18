#!/usr/bin/env python3
import sys
import argparse
import json
from pathlib import Path
import re
from dataclasses import dataclass, field
from datetime import datetime
try:
    from tqdm import tqdm
except ImportError:
    def tqdm(iterable, **kwargs):
        return iterable

# ── Data structures ──────────────────────────────────────────────────────────

@dataclass
class HeapStats:
    samples: list = field(default_factory=list)  # (ts, free, min_free, largest)
    slope_bytes_per_s: float = 0.0
    early_free: int = 0
    end_free: int = 0
    end_min_free: int = 0
    early_min_free: int = 0
    leak_status: str = "NO_DATA"

@dataclass
class MeasureStats:
    boot_ts_us: int = -1                # sensor or receiver
    first_rx_ts_us: int = -1            # receiver only
    airtime_samples: list = field(default_factory=list)   # (ts, util_per_mille, d_airtime_us, d_packets)
    lat_cb_us: list = field(default_factory=list)         # ESP-NOW send callback turnaround microseconds
    lat_cb_fail_count: int = 0
    lat_rtt_ms: list = field(default_factory=list)        # broadcast: app-ACK RTT milliseconds
    task_hwm_min: dict = field(default_factory=dict)      # name -> minimum hwm_bytes seen across samples

@dataclass
class SensorData:
    board_id: str
    port: str
    can_id: str
    can_ids: list = field(default_factory=list)
    generated_counters: list = field(default_factory=list)
    generated_counters_by_can_id: dict = field(default_factory=dict)
    transport_counters: list = field(default_factory=list)
    transport_counters_by_can_id: dict = field(default_factory=dict)
    counters: list = field(default_factory=list)
    counter_times: dict = field(default_factory=dict)
    ack_delays: dict = field(default_factory=dict)
    packet_counters: dict = field(default_factory=dict)
    sent_times: dict = field(default_factory=dict)
    sent_times_by_can_id: dict = field(default_factory=dict)
    crash_count: int = 0
    retry_count: int = 0
    max_retry_chain: int = 0
    max_retry_fail_count: int = 0
    panic_detected: bool = False
    crash_details: list = field(default_factory=list) # (line_num, description)
    queue_push_count: int = 0
    queue_drop_count: int = 0
    queue_drop_counters: set = field(default_factory=set)
    send_queue_drop_count: int = 0
    heap: HeapStats = field(default_factory=HeapStats)
    measure: MeasureStats = field(default_factory=MeasureStats)

@dataclass
class ReceiverData:
    board_id: str
    port: str
    received: dict = field(default_factory=dict)
    received_times: dict = field(default_factory=dict)
    rx_stats: dict = field(default_factory=dict)
    crash_count: int = 0
    panic_detected: bool = False
    crash_details: list = field(default_factory=list) # (line_num, description)
    queue_drop_count: int = 0
    dedup_drop_count: int = 0
    heap: HeapStats = field(default_factory=HeapStats)
    measure: MeasureStats = field(default_factory=MeasureStats)

@dataclass
class ComparisonResult:
    sensor: SensorData
    receiver: ReceiverData
    can_id: str
    sent: list
    received: set
    missed: list

@dataclass
class SensorSummary:
    sensor: SensorData
    can_id: str
    generated: list
    sent: list
    received_by_any: set
    total_misses: list # total misses from generated
    transport_misses: list # misses from generated that didn't reach transport
    network_misses: list # misses from transport that didn't reach any receiver

@dataclass
class AnalysisContext:
    metadata: dict = field(default_factory=dict)
    active_filtering: bool = False
    receiver_allowlists: dict = field(default_factory=dict)
    expected_receivers: dict = field(default_factory=dict)

@dataclass
class AnalysisRunResult:
    folder: Path
    passed: bool
    suite: str
    transport: str
    frequency: str
    topology: str
    repeat: int | None

# ── Parsing ──────────────────────────────────────────────────────────────────

RE_SENSOR_CAN_ID = re.compile(r"(?:CAN ID|Base ID|base=)(?::\s*)?(?:0x([0-9a-fA-F]+)|([0-9a-fA-F]+))", re.IGNORECASE)
RE_SENSOR_CAN_PROC = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?CAN_PROC_\d+:\s*(0x[0-9a-fA-F]+)\s*batch\s*\d+\s*\[(\d+)\.\.(\d+)\]\s*at\s*\((\d+)\)"
)
RE_SENSOR_ACK = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?ACK:\s*Received ACK for packet tick\s*(\d+)"
)
RE_SENSOR_COUNTER = re.compile(
    r"(?:\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?read_data_task:\s*(?:\[0x[0-9a-fA-F]+\]\s*)?(\d+)|S:(\d+):([0-9a-fA-F]+):(\d+))"
)
RE_SENSOR_QUEUE_DROP = re.compile(
    r"read_data_task:\s*(?:\[0x[0-9a-fA-F]+\]\s*)?Send queue full,\s*dropping counter=(\d+)"
)
RE_WCAN_SEND_QUEUE_DROP = re.compile(
    r"send_data:\s*Send queue full,\s*dropping packet with CAN ID\s*0x[0-9a-fA-F]+"
)
RE_RECEIVER_MODE = re.compile(r"RECEIVER mode", re.IGNORECASE)
RE_RECEIVER_BATCH = re.compile(
    r"(?:\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?wcan_recv_callback:\s*\[([0-9a-fA-F]+)\]\s*tick=(\d+)\s*\[(\d+)\.\.(\d+)\]\s*(\d+)\s*items|R:(\d+):([0-9a-fA-F]+):(\d+):(\d+):(\d+):(\d+))"
)
RE_RECEIVER_RX_STATS = re.compile(
    r"RX_STATS:\s*id=0x([0-9a-fA-F]+)\s*packets=(\d+)\s*samples=(\d+)\s*gaps=(\d+)\s*last=\[(\d+)\.\.(\d+)\]"
)
RE_RECEIVER_QUEUE_DROP = re.compile(
    r"Recv queue full,\s*dropping packet \(id:\s*([0-9a-fA-F]+)\)"
)
RE_RECEIVER_DEDUP_DROP = re.compile(
    r"Dropping duplicate id=0x([0-9a-fA-F]+)\s*tc=(\d+)"
)
RE_FILENAME = re.compile(r"(sensor|receiver)_([A-Za-z0-9]+)_(.+)\.log")
RE_HEAP = re.compile(
    r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\].*?HEAP:\s*free=(\d+)\s*min_free=(\d+)\s*largest=(\d+)"
)

# Crash and Panic signatures
RE_PANIC = re.compile(
    r"(Guru Meditation Error|abort\(\) was called|assert failed|Backtrace:)",
    re.IGNORECASE
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
MAX_RETRY_MARKER = "Max retry attempts reached"
RE_RETRY_ATTEMPT = re.compile(r"Timeout reached.*?Attempt:\s*(\d+)\s*of\s*\d+")

LEAK_SLOPE_FAIL = -64.0
LEAK_SLOPE_SUSPECTED = -16.0
LEAK_END_DROP_RATIO = 0.80
LEAK_ABS_FAIL_BYTES = 2048
LEAK_ABS_SUSPECTED_BYTES = 1024
HEAP_INIT_TRANSIENT_SEC = 2.4

def _parse_ts(ts_str: str) -> float:
    return datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S.%f").timestamp()

def _normalize_can_id(can_id: str) -> str:
    raw = can_id.lower().removeprefix("0x")
    return f"{int(raw, 16):x}"

def _format_can_id(can_id: str) -> str:
    return f"0x{int(str(can_id).lower().removeprefix('0x'), 16):x}"

def _format_can_ids(can_ids) -> str:
    ids = sorted({_normalize_can_id(can_id) for can_id in can_ids})
    return ", ".join(_format_can_id(can_id) for can_id in ids) if ids else "none"

def _format_counter_list(values, limit: int = 24) -> str:
    values = sorted(values)
    if not values:
        return "[]"

    ranges = []
    start = prev = values[0]
    for value in values[1:]:
        if value == prev + 1:
            prev = value
            continue
        ranges.append((start, prev))
        start = prev = value
    ranges.append((start, prev))

    rendered = []
    for start, end in ranges[:limit]:
        rendered.append(f"[{start}..{end}]" if start != end else f"[{start}]")

    text = "[" + ", ".join(rendered) + "]"
    if len(ranges) > limit:
        text += f" ({len(ranges) - limit} more ranges)"
    return text

def _receiver_received_for_sent(receiver: ReceiverData, can_id: str, sent) -> set:
    """Return counters considered delivered for a specific sent-counter set.

    Per-packet receiver callback logs can be dropped by UART/host capture under
    load. When RX_STATS reports a gap-free stream, use it as the authoritative
    delivery signal and clamp it to the counters that actually reached CAN_PROC.
    """
    normalized_can_id = _normalize_can_id(can_id)
    sent_set = set(sent)
    stats = receiver.rx_stats.get(normalized_can_id)
    if stats and stats.get("gaps", 0) == 0:
        last = stats.get("last_last", -1)
        return {counter for counter in sent_set if counter <= last}
    return receiver.received.get(normalized_can_id, set()) & sent_set

def _sensor_generated_for_can_id(sensor: SensorData, can_id: str) -> list:
    normalized = _normalize_can_id(can_id)
    values = sensor.generated_counters_by_can_id.get(normalized)
    if values is not None:
        return sorted(values)
    return list(sensor.generated_counters)


def _sensor_transport_for_can_id(sensor: SensorData, can_id: str) -> list:
    normalized = _normalize_can_id(can_id)
    values = sensor.transport_counters_by_can_id.get(normalized)
    if values is not None:
        return sorted(values)
    return list(sensor.transport_counters)


def _receiver_observed_total(receiver: ReceiverData, allowed_can_ids=None) -> int:
    allowed = None
    if allowed_can_ids is not None:
        allowed = {_normalize_can_id(can_id) for can_id in allowed_can_ids}

    if receiver.rx_stats:
        total = 0
        for can_id, stats in receiver.rx_stats.items():
            if allowed is None or _normalize_can_id(can_id) in allowed:
                total += stats.get("samples", 0)
        return total

    total = 0
    for can_id, counters in receiver.received.items():
        if allowed is None or _normalize_can_id(can_id) in allowed:
            total += len(counters)
    return total

def _frequency_label(metadata: dict) -> str:
    sensor_frequencies = metadata.get("sensor_frequencies") or {}
    if sensor_frequencies:
        return ",".join(
            f"{board_id}={int(freq)}Hz"
            for board_id, freq in sorted(sensor_frequencies.items())
        )

    frequency_hz = metadata.get("frequency_hz")
    if frequency_hz is not None:
        return f"{int(frequency_hz)}Hz"

    return "freq=unknown"

def load_analysis_context(test_dir: Path) -> AnalysisContext:
    scenario_path = test_dir / "scenario.json"
    if not scenario_path.exists():
        return AnalysisContext()

    metadata = json.loads(scenario_path.read_text(encoding="utf-8"))
    receiver_allowlists = {
        board_id: {_normalize_can_id(can_id) for can_id in ids}
        for board_id, ids in metadata.get("receiver_allowlists", {}).items()
    }
    expected_receivers = {
        _normalize_can_id(can_id): list(board_ids)
        for can_id, board_ids in metadata.get("expected_receivers", {}).items()
    }
    return AnalysisContext(
        metadata=metadata,
        active_filtering=bool(metadata.get("active_filtering")),
        receiver_allowlists=receiver_allowlists,
        expected_receivers=expected_receivers,
    )

def _topology_and_repeat(test_dir: Path, metadata: dict) -> tuple[str, int | None]:
    topology = str(metadata.get("topology", test_dir.name.removeprefix("test_")))
    repeat = metadata.get("repeat")

    if repeat is None:
        m = re.match(r"(.+)_rep(\d+)$", topology)
        if m:
            return m.group(1), int(m.group(2))
        return topology, None

    return re.sub(rf"_rep{re.escape(str(repeat))}$", "", topology), int(repeat)

def analysis_run_result(test_dir: Path, passed: bool) -> AnalysisRunResult:
    metadata = load_analysis_context(test_dir).metadata
    topology, repeat = _topology_and_repeat(test_dir, metadata)
    return AnalysisRunResult(
        folder=test_dir,
        passed=passed,
        suite=metadata.get("suite", metadata.get("scenario", "unknown")),
        transport=metadata.get("transport", "unknown"),
        frequency=_frequency_label(metadata),
        topology=topology,
        repeat=repeat,
    )

def format_topology_summary(results: list[AnalysisRunResult], min_successful_reps: int = 1) -> str:
    groups = {}
    for result in results:
        key = (result.suite, result.transport, result.frequency, result.topology)
        groups.setdefault(key, []).append(result)

    passed_topologies = 0
    lines = []
    for key in sorted(groups):
        suite, transport, frequency, topology = key
        runs = sorted(groups[key], key=lambda r: (-1 if r.repeat is None else r.repeat, str(r.folder)))
        passed_runs = [run for run in runs if run.passed]
        if len(passed_runs) >= min_successful_reps:
            passed_topologies += 1
            status = "PASS"
        else:
            status = "FAIL"

        passed_reps = [
            f"rep{run.repeat}" if run.repeat is not None else run.folder.name
            for run in passed_runs
        ]
        failed_reps = [
            f"rep{run.repeat}" if run.repeat is not None else run.folder.name
            for run in runs
            if not run.passed
        ]
        detail = f"{len(passed_runs)}/{len(runs)} reps passed"
        if passed_reps:
            detail += f"; pass={','.join(passed_reps)}"
        if failed_reps:
            detail += f"; fail={','.join(failed_reps)}"
        lines.append(f"{suite} | {transport} | {frequency} | {topology} | {status} ({detail})")

    threshold = f">={min_successful_reps}"
    header = f"TOPOLOGY RESULT: {passed_topologies}/{len(groups)} topologies passed ({threshold} successful reps)"
    if not lines:
        return header
    return header + "\n" + "\n".join(lines)

def _receiver_accepts_can_id(context: AnalysisContext, receiver: ReceiverData, can_id: str) -> bool:
    if not context.active_filtering:
        return True
    return _normalize_can_id(can_id) in context.receiver_allowlists.get(receiver.board_id, set())

def _subscribed_receivers(context: AnalysisContext, receivers, can_id: str):
    if not context.active_filtering:
        return list(receivers)
    normalized = _normalize_can_id(can_id)
    receiver_by_id = {r.board_id: r for r in receivers}
    expected = [
        receiver_by_id[board_id]
        for board_id in context.expected_receivers.get(normalized, [])
        if board_id in receiver_by_id
    ]
    if expected:
        return expected
    return [
        receiver
        for receiver in receivers
        if normalized in context.receiver_allowlists.get(receiver.board_id, set())
    ]

def parse_heap_stats(text: str) -> HeapStats:
    samples = []
    for m in RE_HEAP.finditer(text):
        ts = _parse_ts(m.group(1))
        samples.append((ts, int(m.group(2)), int(m.group(3)), int(m.group(4))))

    stats = HeapStats(samples=samples)
    if len(samples) < 3:
        return stats

    t0 = samples[0][0]
    post_init = [
        (t - t0, free, min_free)
        for (t, free, min_free, _largest) in samples
        if (t - t0) >= HEAP_INIT_TRANSIENT_SEC
    ]
    if len(post_init) < 2:
        post_init = [(t - t0, free, min_free) for (t, free, min_free, _largest) in samples]
    if len(post_init) < 2:
        return stats

    n = len(post_init)
    sum_t = sum(t for t, _, _ in post_init)
    sum_y = sum(free for _, free, _ in post_init)
    sum_tt = sum(t * t for t, _, _ in post_init)
    sum_ty = sum(t * free for t, free, _ in post_init)
    denom = n * sum_tt - sum_t * sum_t
    slope = (n * sum_ty - sum_t * sum_y) / denom if denom != 0 else 0.0

    early_free = post_init[0][1]
    end_free = post_init[-1][1]
    early_min_free = post_init[0][2]
    end_min_free = post_init[-1][2]
    stats.slope_bytes_per_s = slope
    stats.early_free = early_free
    stats.end_free = end_free
    stats.early_min_free = early_min_free
    stats.end_min_free = end_min_free

    free_drop = early_free - end_free
    if (slope <= LEAK_SLOPE_FAIL and free_drop >= LEAK_ABS_FAIL_BYTES) or end_free < early_free * LEAK_END_DROP_RATIO:
        stats.leak_status = "FAIL"
    elif slope <= LEAK_SLOPE_SUSPECTED and free_drop >= LEAK_ABS_SUSPECTED_BYTES:
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

def _find_crash_locations(text: str) -> list:
    """Find line numbers and descriptions for crashes and panics."""
    locations = []
    lines = text.splitlines()
    first_boot_found = False
    for i, line in enumerate(lines, 1):
        if CRASH_MARKER in line:
            if not first_boot_found:
                first_boot_found = True
                continue
            locations.append((i, "Reboot (WCAN initialized)"))
        
        panic_match = RE_PANIC.search(line)
        if panic_match:
            locations.append((i, f"Panic ({panic_match.group(0)})"))
            
    return locations

def parse_sensor_log(path: Path) -> SensorData:
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    can_match = RE_SENSOR_CAN_ID.search(text)
    if not can_match:
        # Fallback 1: Look for ID in the first CAN_PROC batch
        can_match = RE_SENSOR_CAN_PROC.search(text)
        if can_match:
            can_id = _normalize_can_id(can_match.group(2))
        else:
            # Fallback 2: Look for ID in the first high-efficiency log line
            can_match = RE_SENSOR_COUNTER.search(text)
            if can_match:
                # Group 4 is ID in S:MS:ID:Val
                can_id = _normalize_can_id(can_match.group(4))
            else:
                raise ValueError(f"No CAN ID found in sensor log: {path.name}")
    else:
        # Pick the group that matched (1 if 0x was present, 2 otherwise)
        can_id_raw = can_match.group(1) or can_match.group(2)
        can_id = _normalize_can_id(can_id_raw)

    counter_times = {}
    generated_by_can_id = {}
    raw_queue_push_count = 0
    can_ids_seen = []
    for m in RE_SENSOR_COUNTER.finditer(text):
        raw_queue_push_count += 1
        # Group 1,2: old format (ts_str, counter); Group 3,4,5: new format (ms, id, counter)
        if m.group(1):
            ts = _parse_ts(m.group(1))
            counter_val = int(m.group(2))
            counter_can_id = can_id
        else:
            ts = int(m.group(3)) / 1000.0 # Convert ms to s
            counter_can_id = _normalize_can_id(m.group(4))
            counter_val = int(m.group(5))
        counter_times[counter_val] = ts
        generated_by_can_id.setdefault(counter_can_id, set()).add(counter_val)
        if counter_can_id not in can_ids_seen:
            can_ids_seen.append(counter_can_id)
    queue_drop_counters = {int(m.group(1)) for m in RE_SENSOR_QUEUE_DROP.finditer(text)}

    can_proc_times = {}
    packet_counters = {}
    sent_times = {}
    sent_times_by_can_id = {}
    sent_counter_set = set()
    transport_by_can_id = {}
    can_proc_sample_count = 0
    saw_can_proc = False
    for m in RE_SENSOR_CAN_PROC.finditer(text):
        saw_can_proc = True
        ts = _parse_ts(m.group(1))
        proc_can_id = _normalize_can_id(m.group(2))
        first_counter = int(m.group(3))
        last_counter = int(m.group(4))
        tick = int(m.group(5))
        can_proc_sample_count += max(0, last_counter - first_counter + 1)
        can_proc_times[tick] = ts
        packet_counters[tick] = first_counter
        for counter in range(first_counter, last_counter + 1):
            sent_counter_set.add(counter)
            transport_by_can_id.setdefault(proc_can_id, set()).add(counter)
            sent_times[counter] = ts
            sent_times_by_can_id.setdefault(proc_can_id, {})[counter] = ts
            counter_times.setdefault(counter, ts)
            generated_by_can_id.setdefault(proc_can_id, set()).add(counter)
        if proc_can_id not in can_ids_seen:
            can_ids_seen.append(proc_can_id)

    # Current firmware emits S:<ms>:<can_id>:<counter> after send_data() succeeds.
    # If there are no CAN_PROC logs, S: is both the generated sample and the
    # best available proof that the sample entered WCAN transport.
    if not saw_can_proc:
        transport_by_can_id = {cid: set(values) for cid, values in generated_by_can_id.items()}
        sent_counter_set = {counter for values in transport_by_can_id.values() for counter in values}

    counters = sorted(sent_counter_set) if sent_counter_set else sorted(counter_times.keys())
    generated_counters = sorted({counter for values in generated_by_can_id.values() for counter in values})
    transport_counters = sorted(sent_counter_set)
    generated_counters_by_can_id = {cid: sorted(values) for cid, values in generated_by_can_id.items()}
    transport_counters_by_can_id = {cid: sorted(values) for cid, values in transport_by_can_id.items()}
    queue_push_count = can_proc_sample_count if can_proc_sample_count else raw_queue_push_count
    can_ids = can_ids_seen or [can_id]

    ack_delays = {}
    for m in RE_SENSOR_ACK.finditer(text):
        ts = _parse_ts(m.group(1))
        tick = int(m.group(2))
        if tick in can_proc_times:
            ack_delays[tick] = (ts - can_proc_times[tick]) * 1000

    crash_count = max(0, text.count(CRASH_MARKER) - 1)
    panic_detected = bool(RE_PANIC.search(text))
    crash_details = _find_crash_locations(text)
    retry_count = text.count(RETRY_MARKER)
    max_retry_fail_count = text.count(MAX_RETRY_MARKER)
    max_retry_chain = max((int(m.group(1)) for m in RE_RETRY_ATTEMPT.finditer(text)), default=0)

    return SensorData(
        board_id=board_id,
        port=port,
        can_id=can_id,
        can_ids=can_ids,
        generated_counters=generated_counters,
        generated_counters_by_can_id=generated_counters_by_can_id,
        transport_counters=transport_counters,
        transport_counters_by_can_id=transport_counters_by_can_id,
        counters=counters,
        counter_times=counter_times,
        ack_delays=ack_delays,
        packet_counters=packet_counters,
        sent_times=sent_times,
        sent_times_by_can_id=sent_times_by_can_id,
        crash_count=crash_count,
        panic_detected=panic_detected,
        crash_details=crash_details,
        retry_count=retry_count,
        max_retry_chain=max_retry_chain,
        max_retry_fail_count=max_retry_fail_count,
        queue_push_count=queue_push_count,
        queue_drop_count=len(RE_SENSOR_QUEUE_DROP.findall(text)),
        queue_drop_counters=queue_drop_counters,
        send_queue_drop_count=len(RE_WCAN_SEND_QUEUE_DROP.findall(text)),
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
        # Group 1-6: old format (ts_str, id, tick, first, last, count)
        # Group 7-12: new format (ms, id, tick, first, last, count)
        if m.group(1):
            ts = _parse_ts(m.group(1))
            can_id = _normalize_can_id(m.group(2))
            first, last = int(m.group(4)), int(m.group(5))
        else:
            ts = int(m.group(7)) / 1000.0
            can_id = _normalize_can_id(m.group(8))
            first, last = int(m.group(10)), int(m.group(11))
        received.setdefault(can_id, set()).update(range(first, last + 1))
        received_times.setdefault(can_id, {})[first] = ts

    rx_stats = {}
    for m in RE_RECEIVER_RX_STATS.finditer(text):
        can_id = _normalize_can_id(m.group(1))
        rx_stats[can_id] = {
            "packets": int(m.group(2)),
            "samples": int(m.group(3)),
            "gaps": int(m.group(4)),
            "last_first": int(m.group(5)),
            "last_last": int(m.group(6)),
        }

    crash_count = max(0, text.count(CRASH_MARKER) - 1)
    panic_detected = bool(RE_PANIC.search(text))
    crash_details = _find_crash_locations(text)
    queue_drop_count = len(RE_RECEIVER_QUEUE_DROP.findall(text))
    dedup_drop_count = len(RE_RECEIVER_DEDUP_DROP.findall(text))

    return ReceiverData(
        board_id=board_id,
        port=port,
        received=received,
        received_times=received_times,
        rx_stats=rx_stats,
        crash_count=crash_count,
        panic_detected=panic_detected,
        crash_details=crash_details,
        queue_drop_count=queue_drop_count,
        dedup_drop_count=dedup_drop_count,
        heap=parse_heap_stats(text),
        measure=parse_measure(text),
    )

def compare(sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    results = []
    for sensor in sensors:
        for can_id in sensor.can_ids:
            sent = _sensor_transport_for_can_id(sensor, can_id)
            if sent:
                sent = sent[:-1]
            for receiver in receivers:
                if not _receiver_accepts_can_id(context, receiver, can_id):
                    results.append(
                        ComparisonResult(
                            sensor=sensor,
                            receiver=receiver,
                            can_id=can_id,
                            sent=[],
                            received=set(),
                            missed=[],
                        )
                    )
                    continue
                recv_counters = _receiver_received_for_sent(receiver, can_id, sent)

                missed_set = set(sent) - recv_counters
                n_tail = 0
                for c in reversed(sent):
                    if c in missed_set:
                        n_tail += 1
                    else:
                        break
                trimmed_sent = sent[:-n_tail] if 0 < n_tail < len(sent) else sent

                missed = sorted(c for c in trimmed_sent if c not in recv_counters)
                results.append(
                    ComparisonResult(
                        sensor=sensor,
                        receiver=receiver,
                        can_id=can_id,
                        sent=trimmed_sent,
                        received=recv_counters & set(trimmed_sent),
                        missed=missed,
                    )
                )
    return results

def is_test_folder(path: Path) -> bool:
    return any(path.glob("sensor_*.log")) and any(path.glob("receiver_*.log"))

def find_test_folders(root: Path) -> list:
    if is_test_folder(root):
        return [root]

    folders = sorted(p for p in root.rglob("test_*") if p.is_dir() and is_test_folder(p))
    if not folders:
        print(f"No test folders found in {root}")
        sys.exit(1)
    return folders

#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path

def sensor_summary(sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    summaries = []
    for sensor in sensors:
        for can_id in sensor.can_ids:
            generated = _sensor_generated_for_can_id(sensor, can_id)
            if generated:
                generated = generated[:-1]
            sent = _sensor_transport_for_can_id(sensor, can_id)
            union = set()
            subscribed = _subscribed_receivers(context, receivers, can_id)
            if context.active_filtering and not subscribed:
                summaries.append(
                    SensorSummary(
                        sensor=sensor,
                        can_id=can_id,
                        generated=[],
                        sent=[],
                        received_by_any=set(),
                        total_misses=[],
                        transport_misses=[],
                        network_misses=[],
                    )
                )
                continue

            for receiver in subscribed:
                union |= _receiver_received_for_sent(receiver, can_id, generated)

            # misses from generated
            missed_set = set(generated) - union
            
            # Tail trimming: ignore misses at the very end of the test as they might
            # just be data that hasn't arrived/logged before shutdown.
            # Boundary is the last counter actually confirmed to have reached a sink.
            n_tail = 0
            for c in reversed(generated):
                if c in missed_set:
                    n_tail += 1
                else:
                    break
            
            trimmed_generated = generated[:-n_tail] if 0 < n_tail < len(generated) else generated
            trimmed_union = union & set(trimmed_generated)
            trimmed_misses = sorted(set(trimmed_generated) - trimmed_union)
            
            # transport misses: generated but not in transport_counters AND not in union
            # (If it arrived at the sink, it obviously reached transport, even if the sensor log was dropped)
            sent_set = set(sent)
            transport_misses = sorted(c for c in trimmed_generated if c not in sent_set and c not in union)
            
            # network misses: in transport_counters but not received (real radio losses)
            network_misses = sorted(c for c in sent if c in set(trimmed_generated) and c not in trimmed_union)

            summaries.append(
                SensorSummary(
                    sensor=sensor,
                    can_id=can_id,
                    generated=trimmed_generated,
                    sent=sent,
                    received_by_any=trimmed_union,
                    total_misses=trimmed_misses,
                    transport_misses=transport_misses,
                    network_misses=network_misses,
                )
            )
    return summaries

def analyze_delivery(sensors, receivers, test_name, context: AnalysisContext = None):
    """Unified Source-to-Sink delivery analysis."""
    context = context or AnalysisContext()
    summaries = sensor_summary(sensors, receivers, context)
    report_lines = []
    
    total_generated = sum(len(s.generated) for s in summaries)
    total_received = sum(len(s.received_by_any) for s in summaries)
    total_misses = sum(len(s.total_misses) for s in summaries)
    
    # Categorize misses. Only explicit firmware queue-full logs are called
    # producer drops; inferred gaps before radio are reported separately.
    total_producer_drops = sum(sensor.queue_drop_count for sensor in sensors)
    total_pre_radio_misses = sum(len(s.transport_misses) for s in summaries)
    total_send_queue_drops = sum(sensor.send_queue_drop_count for sensor in sensors)
    total_network_misses = sum(len(s.network_misses) for s in summaries)
    total_recv_queue_drops = sum(r.queue_drop_count for r in receivers)
    
    status = "PASS" if total_misses == 0 else "FAIL"
    delivery_pct = (100.0 * total_received / total_generated) if total_generated else 100.0
    
    report_lines.append(f"DELIVERY ANALYSIS: {status} ({delivery_pct:.2f}% end-to-end delivery)")
    report_lines.append(f"  Summary: {total_generated} generated, {total_received} received, {total_misses} total misses")
    report_lines.append(f"  Loss Breakdown:")
    report_lines.append(f"    1. Producer Drops:   {total_producer_drops} (explicit read_data_task -> WCAN queue full logs)")
    if total_pre_radio_misses:
        report_lines.append(f"       Pre-radio Unobserved: {total_pre_radio_misses} (generated but no transport proof in logs)")
    report_lines.append(f"    2. Send Queue Drops: {total_send_queue_drops} (WCAN -> Radio queue full)")
    report_lines.append(f"    3. Radio/ACK Losses: {total_network_misses} (Sent but never seen by any receiver)")
    report_lines.append(f"    4. Receiver Drops:   {total_recv_queue_drops} (Received but app recv_queue full)")

    for s in summaries:
        sensor = s.sensor
        n_gen = len(s.generated)
        n_recv = len(s.received_by_any)
        n_miss = len(s.total_misses)
        retries_str = f"{sensor.retry_count}({sensor.max_retry_chain})" if sensor.max_retry_chain > 1 else f"{sensor.retry_count}"
        
        line = (
            f"Sensor {sensor.board_id} (0x{s.can_id} | {sensor.port}): "
            f"{n_gen} generated, {n_recv} received, {n_miss} misses, {retries_str} retries"
        )
        report_lines.append(line)
        if s.transport_misses:
             report_lines.append(f"   - PRE-RADIO UNOBSERVED ({len(s.transport_misses)}): {_format_counter_list(s.transport_misses)}")
        if s.network_misses:
             report_lines.append(f"   - RADIO LOSSES ({len(s.network_misses)}): {_format_counter_list(s.network_misses)}")
        if sensor.send_queue_drop_count > 0:
             report_lines.append(f"   - WCAN SEND QUEUE DROPS: {sensor.send_queue_drop_count} batches")

    for receiver in receivers:
        if receiver.queue_drop_count > 0 or receiver.dedup_drop_count > 0:
            report_lines.append(
                f"Receiver {receiver.board_id} ({receiver.port}): {receiver.queue_drop_count} queue drops, "
                f"{receiver.dedup_drop_count} duplicate drops"
            )

    passed = total_misses == 0
    return "\n".join(report_lines), passed

def analyze_crashes(sensors, receivers):
    """Analyze if there were crashes or panics with line numbers."""
    report_lines = []
    total_incidents = 0
    
    for device in sensors + receivers:
        if device.crash_details:
            total_incidents += len(device.crash_details)
            dev_name = f"{device.__class__.__name__} {device.board_id}"
            for line_num, desc in device.crash_details:
                report_lines.append(f"{dev_name}: {desc} at line {line_num}")
            
    passed = total_incidents == 0
    status = "PASS" if passed else "FAIL"
    report_lines.insert(0, f"CRASH ANALYSIS: {status} ({total_incidents} total incidents)")
    return "\n".join(report_lines), passed

def _format_heap(stats) -> str:
    if stats.leak_status == "NO_DATA":
        return "no_samples"
    return (
        f"{stats.leak_status} (slope={stats.slope_bytes_per_s:+.1f} B/s, "
        f"free {stats.early_free}->{stats.end_free}, "
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
        latencies = []
        if m.lat_cb_us:
            latencies.extend([us / 1000.0 for us in m.lat_cb_us])
        if m.lat_rtt_ms:
            latencies.extend([float(ms) for ms in m.lat_rtt_ms])
        
        if latencies:
            xs = sorted(latencies)
            report_lines.append(
                f"  Sensor {s.board_id} Latency (ms): "
                f"n={len(xs)} median={_percentile(xs, 0.5):.2f}ms p99={_percentile(xs, 0.99):.2f}ms "
                f"max={xs[-1]:.2f}ms"
            )
            if m.lat_cb_fail_count:
                 report_lines.append(f"    (MAC failures: {m.lat_cb_fail_count})")

        if m.airtime_samples:
            utils = [u for (_, u, _, _) in m.airtime_samples]
            channel_time_pct = (sum(utils) / len(utils)) / 10.0
            d_packets_total = sum(p for (_, _, _, p) in m.airtime_samples)
            d_airtime_total = sum(a for (_, _, a, _) in m.airtime_samples)
            report_lines.append(
                f"  Sensor {s.board_id} channel time: avg={channel_time_pct:.2f}% "
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
            hwm = all_task_hwm[name]
            warning = " [LOW STACK WARNING]" if hwm < 128 else ""
            report_lines.append(f"    {name}: {hwm} bytes free{warning}")

    return "\n".join(report_lines), True

def _trim_tail(sent: list, received: set) -> list:
    missed_set = set(sent) - received
    n_tail = 0
    for c in reversed(sent):
        if c in missed_set:
            n_tail += 1
        else:
            break
    return sent[:-n_tail] if 0 < n_tail < len(sent) else sent

def _sensor_observed_rate_hz(sensor: SensorData):
    counters = sensor.counters
    if len(counters) < 2:
        return None
    first_counter = counters[0]
    last_counter = counters[-1]
    first_ts = sensor.counter_times.get(first_counter)
    last_ts = sensor.counter_times.get(last_counter)
    if first_ts is None or last_ts is None or last_ts <= first_ts:
        return None
    return (last_counter - first_counter) / (last_ts - first_ts)

def analyze_scenario(test_dir: Path, sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    if not context.metadata:
        return "SCENARIO ANALYSIS: none", True

    metadata = context.metadata
    scenario = metadata.get("scenario", "baseline")
    
    report_lines = []
    failures = 0
    receiver_by_id = {r.board_id: r for r in receivers}

    # 1. Active Filtering Check (Scenario specific)
    if metadata.get("active_filtering"):
        allowlists = context.receiver_allowlists
        expected_receivers = context.expected_receivers
        report_lines.append("Active filter delivery:")

        for receiver in receivers:
            allowed = allowlists.get(receiver.board_id, set())
            unexpected_ids = sorted(
                can_id for can_id, counters in receiver.received.items()
                if counters and can_id not in allowed
            )
            if unexpected_ids:
                failures += len(unexpected_ids)
                report_lines.append(
                    f"Receiver {receiver.board_id}: unexpected CAN IDs {_format_can_ids(unexpected_ids)}"
                )

        sensors_by_can_id = {
            _normalize_can_id(can_id): sensor
            for sensor in sensors
            for can_id in sensor.can_ids
        }
        for receiver in receivers:
            allowed = sorted(allowlists.get(receiver.board_id, set()))
            if not allowed:
                observed_total = _receiver_observed_total(receiver)
                report_lines.append(
                    f"RECEIVER {receiver.board_id} -> expected none -> received {observed_total} total"
                )
                continue
            for can_id in allowed:
                sensor = sensors_by_can_id.get(_normalize_can_id(can_id))
                if sensor is None:
                    failures += 1
                    report_lines.append(
                        f"RECEIVER {receiver.board_id} -> expected {_format_can_id(can_id)} -> no matching sensor log"
                    )
                    continue
                sent = _sensor_transport_for_can_id(sensor, can_id)
                if sent:
                    sent = sent[:-1]
                received = _receiver_received_for_sent(receiver, can_id, sent)
                trimmed_sent = _trim_tail(sent, received)
                received_count = len(received & set(trimmed_sent))
                missed = sorted(c for c in trimmed_sent if c not in received)
                report_lines.append(
                    f"RECEIVER {receiver.board_id} -> expected {_format_can_id(can_id)} -> "
                    f"received {received_count} from {len(trimmed_sent)} sent from SENSOR {sensor.board_id}"
                )
                if missed:
                    failures += len(missed)
                    report_lines.append(
                        f"RECEIVER {receiver.board_id} -> {_format_can_id(can_id)} missed "
                        f"{_format_counter_list(missed)}"
                    )

        for sensor in sensors:
            for can_id in sensor.can_ids:
                sent = _sensor_transport_for_can_id(sensor, can_id)
                if sent:
                    sent = sent[:-1]
                normalized_can_id = _normalize_can_id(can_id)
                subscriber_ids = [
                    board_id for board_id in expected_receivers.get(normalized_can_id, [])
                    if board_id in receiver_by_id
                ]
                if not subscriber_ids:
                    subscriber_ids = [
                        board_id for board_id, allowed in allowlists.items()
                        if normalized_can_id in allowed and board_id in receiver_by_id
                    ]
                subscribers = [receiver_by_id[board_id] for board_id in subscriber_ids]
                filtered_ids = [
                    receiver.board_id for receiver in receivers
                    if receiver.board_id not in subscriber_ids
                ]
                if not subscribers:
                    report_lines.append(
                        f"SENSOR {sensor.board_id} -> {_format_can_id(can_id)} filtered by all receivers -> expected no receive"
                    )
                    continue
                if filtered_ids:
                    leaked_to = [
                        receiver.board_id for receiver in receivers
                        if receiver.board_id in filtered_ids
                        and _receiver_observed_total(receiver, [normalized_can_id]) > 0
                    ]
                    report_lines.append(
                        f"SENSOR {sensor.board_id} -> {_format_can_id(can_id)} filtered by "
                        f"{', '.join(filtered_ids)} -> {'unexpected receive by ' + ', '.join(leaked_to) if leaked_to else 'no receive'}"
                    )
                    failures += len(leaked_to)
        report_lines.append("Active filtering checks complete")

    # 2. Mandatory Frequency Check (Always active for all sensors)
    tolerance = float(metadata.get("frequency_tolerance", 0.05))
    expected_freqs = {
        board_id: float(freq)
        for board_id, freq in metadata.get("sensor_frequencies", {}).items()
    }
    global_freq = metadata.get("frequency_hz")
    
    for sensor in sensors:
        expected = expected_freqs.get(sensor.board_id) or (float(global_freq) if global_freq is not None else None)
        
        if expected is None:
            continue
            
        observed = _sensor_observed_rate_hz(sensor)
        if observed is None:
            failures += 1
            report_lines.append(f"Sensor {sensor.board_id}: insufficient counter timestamps for rate check")
            continue
            
        low = expected * (1.0 - tolerance)
        high = expected * (1.0 + tolerance)
        ok = low <= observed <= high
        if not ok:
            failures += 1
        report_lines.append(
            f"Sensor {sensor.board_id}: expected {expected:.1f}Hz, observed {observed:.1f}Hz "
            f"({'OK' if ok else 'FAIL'}, tolerance ±{tolerance * 100:.0f}%)"
        )

    passed = failures == 0
    status = "PASS" if passed else "FAIL"
    report_lines.insert(0, f"SCENARIO ANALYSIS: {status} ({scenario}, failures={failures})")
    return "\n".join(report_lines), passed

def analyze_all(test_dir: Path):
    """Run all analysis functions on a test directory."""
    test_name = test_dir.name
    context = load_analysis_context(test_dir)

    sensors = []
    for path in test_dir.glob("sensor_*.log"):
        try:
            sensors.append(parse_sensor_log(path))
        except ValueError as exc:
            print(f"[WARN] Skipping {path.name}: {exc}")
    sensors.sort(key=lambda s: s.board_id)

    receivers = []
    for path in test_dir.glob("receiver_*.log"):
        try:
            receivers.append(parse_receiver_log(path))
        except ValueError as exc:
            print(f"[WARN] Skipping {path.name}: {exc}")
    receivers.sort(key=lambda r: r.board_id)

    if not sensors or not receivers:
         return f"No logs found in {test_dir}\n", False

    out = []
    
    r_delivery, p_delivery = analyze_delivery(sensors, receivers, test_name, context)
    r_crash, p_crash = analyze_crashes(sensors, receivers)
    r_heap, p_heap = analyze_heap_integrity(sensors, receivers)
    r_scenario, p_scenario = analyze_scenario(test_dir, sensors, receivers, context)
    r_measure, _ = analyze_measure(sensors, receivers)

    out.append(r_delivery)
    out.append("")
    out.append(r_crash)
    out.append("")
    out.append(r_heap)
    out.append("")
    out.append(r_scenario)
    out.append("")
    out.append(r_measure)
    
    checks = {
        "delivery": p_delivery,
        "crash": p_crash,
        "heap": p_heap,
        "scenario": p_scenario,
    }
    overall_passed = all(checks.values())
    failed = [name for name, ok in checks.items() if not ok]
    metadata = context.metadata
    topology = metadata.get("topology", test_name.removeprefix("test_"))
    repeat = metadata.get("repeat")
    if repeat is not None and not str(topology).endswith(f"_rep{repeat}"):
        topology = f"{topology}_rep{repeat}"
    suite = metadata.get("suite", metadata.get("scenario", "unknown"))
    transport = metadata.get("transport", "unknown")
    frequency = _frequency_label(metadata)
    status = "PASS" if overall_passed else "FAIL"
    out.insert(0, f"failed: {', '.join(failed) if failed else 'none'}")
    out.insert(0, f"{suite} | {transport} | {frequency} | {topology} | {status}")
    out.append("")
    out.append("======================")
    return "\n".join(out), overall_passed

def main():
    parser = argparse.ArgumentParser(description="WCAN Log Analyzer")
    parser.add_argument("results_path", help="Path to results directory")
    parser.add_argument(
        "--min-successful-reps",
        type=int,
        default=1,
        help="Minimum passing repetitions required for a topology-level PASS.",
    )
    args = parser.parse_args()
    if args.min_successful_reps < 1:
        print("--min-successful-reps must be at least 1")
        sys.exit(1)

    root = Path(args.results_path)
    if not root.exists():
        print(f"Path not found: {root}")
        sys.exit(1)

    test_folders = find_test_folders(root)
    n_passed = 0
    all_reports = []
    run_results = []

    for folder in tqdm(test_folders, desc="Analyzing tests", dynamic_ncols=True):
        report, passed = analyze_all(folder)
        all_reports.append(report)
        run_results.append(analysis_run_result(folder, passed))
        
        # Write per-repetition analysis.txt
        analysis_path = folder / "analysis.txt"
        analysis_path.write_text(report, encoding="utf-8")
        
        if passed:
            n_passed += 1

    result_line = f"RESULT: {n_passed}/{len(test_folders)} tests passed"
    topology_summary = format_topology_summary(run_results, args.min_successful_reps)
    
    # Final combined report string
    combined_report = f"{result_line}\n{topology_summary}\n\n" + "======================\n\n" + "\n\n".join(all_reports)

    # Print to console
    print(combined_report)

    if len(test_folders) > 1:
        summary_path = root / "analysis_summary.txt"
        summary_path.write_text(combined_report, encoding="utf-8")
        print(f"\nCombined summary saved: {summary_path}")

if __name__ == "__main__":
    main()
