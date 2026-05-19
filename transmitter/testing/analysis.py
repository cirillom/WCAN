#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class MeasureStats:
    values: dict = field(default_factory=dict)
    airtime_samples: list = field(default_factory=list)
    lat_cb_us: list = field(default_factory=list)
    lat_cb_fail_count: int = 0
    lat_rtt_ms: list = field(default_factory=list)
    boot_ts_us: int = -1
    first_rx_ts_us: int = -1
    task_hwm_min: dict = field(default_factory=dict)


@dataclass
class BatchStats:
    values: dict = field(default_factory=dict)


@dataclass
class SensorData:
    board_id: str
    port: str
    can_id: str
    can_ids: list = field(default_factory=list)
    generated_last: int | None = None
    avg_hz: float | None = None
    generated_counters: list = field(default_factory=list)
    generated_counters_by_can_id: dict = field(default_factory=dict)
    transport_counters: list = field(default_factory=list)
    transport_counters_by_can_id: dict = field(default_factory=dict)
    counters: list = field(default_factory=list)
    crash_details: list = field(default_factory=list)
    panic_detected: bool = False
    measure: MeasureStats = field(default_factory=MeasureStats)
    queue_drop_count: int = 0
    producer_drop_count: int = 0
    send_queue_drop_count: int = 0
    retry_count_by_can_id: dict = field(default_factory=dict)
    max_retry_chain_by_can_id: dict = field(default_factory=dict)
    failed_packets_by_can_id: dict = field(default_factory=dict)
    p_fail_count: int = 0
    p_full_count: int = 0
    s_fail_count: int = 0
    batch_stats: dict = field(default_factory=dict)


@dataclass
class ReceiverData:
    board_id: str
    port: str
    received: dict = field(default_factory=dict)
    ranges: dict = field(default_factory=dict)
    crash_details: list = field(default_factory=list)
    panic_detected: bool = False
    measure: MeasureStats = field(default_factory=MeasureStats)
    queue_drop_count: int = 0
    dedup_drop_count: int = 0
    p_fail_count: int = 0
    p_full_count: int = 0
    s_fail_count: int = 0
    batch_stats: dict = field(default_factory=dict)


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
    total_misses: list
    transport_misses: list
    network_misses: list
    delivery_expected: bool = True
    note: str = ""


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


RE_FILENAME = re.compile(r"(sensor|receiver|idle)_([A-Za-z0-9]+)_(.+)\.log")
RE_SENSOR_END = re.compile(r"WCAN_SENSOR_END\s+generated=(\S+)\s+avg_hz=([0-9.]+)\s")
RE_RX_RANGE = re.compile(r"WCAN_RX_RANGE\s+id=0x([0-9a-fA-F]+)\s+ranges=(.*)")
RE_RANGE_ITEM = re.compile(r"\[(\d+)\.\.(\d+)\]")
RE_MEASURES = re.compile(r"WCAN_MEASURES\s+(.*)")
RE_BATCH = re.compile(r"WCAN_BATCH\s+id=0x([0-9a-fA-F]+)\s+(.*)")
RE_ABORT = re.compile(r"WCAN_TEST_ABORT(?:\s+(.+))?")
RE_PANIC = re.compile(r"(Guru Meditation Error|abort\(\) was called|assert failed|Backtrace:)", re.IGNORECASE)
RE_P_FAIL = re.compile(r"\bP\(FAIL\)")
RE_P_FULL = re.compile(r"\bP\(FULL\)")
RE_S_FAIL = re.compile(r"\bS\(FAIL\)")


def _normalize_can_id(can_id) -> str:
    return f"{int(str(can_id).lower().removeprefix('0x'), 16):x}"


def _format_can_id(can_id) -> str:
    return f"0x{int(str(can_id).lower().removeprefix('0x'), 16):x}"


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
    rendered = [f"[{a}..{b}]" if a != b else f"[{a}]" for a, b in ranges[:limit]]
    text = "[" + ", ".join(rendered) + "]"
    if len(ranges) > limit:
        text += f" ({len(ranges) - limit} more ranges)"
    return text


def _percentile(sorted_values: list, fraction: float):
    if not sorted_values:
        return None
    if fraction <= 0:
        return sorted_values[0]
    if fraction >= 1:
        return sorted_values[-1]
    return sorted_values[int(round(fraction * (len(sorted_values) - 1)))]


def parse_filename(path: Path):
    m = RE_FILENAME.match(path.name)
    if not m:
        raise ValueError(f"Unexpected filename format: {path.name}")
    return m.group(1), m.group(2), m.group(3)


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


def _metadata_for_log(path: Path) -> dict:
    return load_analysis_context(path.parent).metadata


def _board_can_ids(metadata: dict, board_id: str) -> list[str]:
    board = metadata.get("boards", {}).get(board_id, {})
    can_ids = board.get("can_ids") or ([board.get("can_id")] if board.get("can_id") else [])
    return [_normalize_can_id(can_id) for can_id in can_ids]


def parse_measures(text: str) -> MeasureStats:
    stats = MeasureStats()
    for m in RE_MEASURES.finditer(text):
        for token in m.group(1).split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            stats.values[key] = value
    if "airtime_us_total" in stats.values or "packets_sent_total" in stats.values:
        stats.airtime_samples.append((0.0, 0, int(stats.values.get("airtime_us_total", 0)), int(stats.values.get("packets_sent_total", 0))))
    return stats


def parse_batch_stats(text: str) -> dict:
    stats = {}
    for m in RE_BATCH.finditer(text):
        values = {}
        for token in m.group(2).split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            values[key] = value
        stats[_normalize_can_id(m.group(1))] = BatchStats(values)
    return stats


def _find_crash_locations(text: str) -> list:
    locations = []
    for i, line in enumerate(text.splitlines(), 1):
        if RE_ABORT.search(line):
            locations.append((i, line.strip()))
        panic = RE_PANIC.search(line)
        if panic:
            locations.append((i, f"Panic ({panic.group(1)})"))
    return locations


def parse_sensor_log(path: Path) -> SensorData:
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    metadata = _metadata_for_log(path)
    can_ids = _board_can_ids(metadata, board_id)
    if not can_ids:
        raise ValueError(f"No CAN IDs found in scenario metadata for sensor {board_id}")

    m = RE_SENSOR_END.search(text)
    if not m:
        raise ValueError(f"No WCAN_SENSOR_END found in sensor log: {path}")

    generated_token = m.group(1)
    generated_last = None if generated_token == "none" else int(generated_token)
    generated = [] if generated_last is None else list(range(0, generated_last + 1))
    generated_by_can_id = {can_id: list(generated) for can_id in can_ids}

    return SensorData(
        board_id=board_id,
        port=port,
        can_id=can_ids[0],
        can_ids=can_ids,
        generated_last=generated_last,
        avg_hz=float(m.group(2)),
        generated_counters=list(generated),
        generated_counters_by_can_id=generated_by_can_id,
        transport_counters=list(generated),
        transport_counters_by_can_id={can_id: list(generated) for can_id in can_ids},
        counters=list(generated),
        crash_details=_find_crash_locations(text),
        panic_detected=bool(RE_PANIC.search(text)),
        measure=parse_measures(text),
        batch_stats=parse_batch_stats(text),
        p_fail_count=len(RE_P_FAIL.findall(text)),
        p_full_count=len(RE_P_FULL.findall(text)),
        s_fail_count=len(RE_S_FAIL.findall(text)),
    )


def _expand_ranges(ranges: list[tuple[int, int]]) -> set[int]:
    values = set()
    for start, end in ranges:
        if end >= start:
            values.update(range(start, end + 1))
    return values


def parse_receiver_log(path: Path) -> ReceiverData:
    _, board_id, port = parse_filename(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    ranges_by_id = {}
    received = {}
    for m in RE_RX_RANGE.finditer(text):
        can_id = _normalize_can_id(m.group(1))
        ranges = [(int(a), int(b)) for a, b in RE_RANGE_ITEM.findall(m.group(2))]
        ranges_by_id.setdefault(can_id, []).extend(ranges)
        received.setdefault(can_id, set()).update(_expand_ranges(ranges))

    return ReceiverData(
        board_id=board_id,
        port=port,
        received=received,
        ranges=ranges_by_id,
        crash_details=_find_crash_locations(text),
        panic_detected=bool(RE_PANIC.search(text)),
        measure=parse_measures(text),
        batch_stats=parse_batch_stats(text),
        p_fail_count=len(RE_P_FAIL.findall(text)),
        p_full_count=len(RE_P_FULL.findall(text)),
        s_fail_count=len(RE_S_FAIL.findall(text)),
    )


def _sensor_generated_for_can_id(sensor: SensorData, can_id: str) -> list:
    return list(sensor.generated_counters_by_can_id.get(_normalize_can_id(can_id), []))


def _sensor_transport_for_can_id(sensor: SensorData, can_id: str) -> list:
    return list(sensor.transport_counters_by_can_id.get(_normalize_can_id(can_id), []))


def _expected_receiver_ids(context: AnalysisContext, can_id: str):
    normalized = _normalize_can_id(can_id)
    if context.active_filtering and normalized in context.expected_receivers:
        return list(context.expected_receivers.get(normalized, []))
    return None


def _receiver_accepts_can_id(context: AnalysisContext, receiver: ReceiverData, can_id: str) -> bool:
    if not context.active_filtering:
        return True
    expected = _expected_receiver_ids(context, can_id)
    if expected is not None:
        return receiver.board_id in expected
    return _normalize_can_id(can_id) in context.receiver_allowlists.get(receiver.board_id, set())


def _subscribed_receivers(context: AnalysisContext, receivers, can_id: str):
    if not context.active_filtering:
        return list(receivers)
    normalized = _normalize_can_id(can_id)
    receiver_by_id = {r.board_id: r for r in receivers}
    expected = _expected_receiver_ids(context, normalized)
    if expected is not None:
        return [receiver_by_id[bid] for bid in expected if bid in receiver_by_id]
    return [r for r in receivers if normalized in context.receiver_allowlists.get(r.board_id, set())]




def _trim_tail(counters: list, received: set) -> list:
    if not counters:
        return counters
    missed = set(counters) - received
    n_tail = 0
    for counter in reversed(counters):
        if counter in missed:
            n_tail += 1
        else:
            break
    return counters[:-n_tail] if 0 < n_tail < len(counters) else counters

def compare(sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    results = []
    for sensor in sensors:
        for can_id in sensor.can_ids:
            sent = _sensor_transport_for_can_id(sensor, can_id)
            for receiver in receivers:
                if not _receiver_accepts_can_id(context, receiver, can_id):
                    results.append(ComparisonResult(sensor, receiver, can_id, [], set(), []))
                    continue
                received = receiver.received.get(_normalize_can_id(can_id), set()) & set(sent)
                trimmed_sent = _trim_tail(sent, received)
                received = received & set(trimmed_sent)
                missed = sorted(set(trimmed_sent) - received)
                results.append(ComparisonResult(sensor, receiver, can_id, trimmed_sent, received, missed))
    return results


def sensor_summary(sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    summaries = []
    for sensor in sensors:
        for can_id in sensor.can_ids:
            generated = _sensor_generated_for_can_id(sensor, can_id)
            sent = _sensor_transport_for_can_id(sensor, can_id)
            subscribed = _subscribed_receivers(context, receivers, can_id)
            expected = _expected_receiver_ids(context, can_id)
            delivery_expected = not (context.active_filtering and expected is not None and not expected)
            if not delivery_expected:
                summaries.append(SensorSummary(
                    sensor, can_id, generated, sent, set(), [], [], [], False,
                    "no expected receivers; excluded from delivery scoring",
                ))
                continue
            union = set()
            for receiver in subscribed:
                union |= receiver.received.get(_normalize_can_id(can_id), set())
            union &= set(generated)
            trimmed_generated = _trim_tail(generated, union)
            trimmed_generated_set = set(trimmed_generated)
            trimmed_union = union & trimmed_generated_set
            trimmed_sent = [counter for counter in sent if counter in trimmed_generated_set]
            total_misses = sorted(trimmed_generated_set - trimmed_union)
            network_misses = sorted(set(trimmed_sent) - trimmed_union)
            summaries.append(SensorSummary(sensor, can_id, trimmed_generated, trimmed_sent, trimmed_union, total_misses, [], network_misses))
    return summaries


def analyze_delivery(sensors, receivers, test_name, context: AnalysisContext = None):
    summaries = sensor_summary(sensors, receivers, context)
    scored = [s for s in summaries if s.delivery_expected]
    total_generated = sum(len(s.generated) for s in scored)
    total_received = sum(len(s.received_by_any) for s in scored)
    total_misses = sum(len(s.total_misses) for s in scored)
    delivery_pct = (100.0 * total_received / total_generated) if total_generated else 100.0
    passed = total_misses == 0
    lines = [f"DELIVERY ANALYSIS: {'PASS' if passed else 'FAIL'} ({delivery_pct:.2f}% end-to-end delivery)"]
    lines.append(f"  Summary: {total_generated} scored generated, {total_received} received, {total_misses} total misses")
    excluded = sum(1 for s in summaries if not s.delivery_expected)
    if excluded:
        lines.append(f"  Excluded: {excluded} sensor/CAN streams with no expected receivers")
    for s in summaries:
        suffix = f", {len(s.total_misses)} misses" if s.delivery_expected else f", {s.note}"
        lines.append(
            f"  Sensor {s.sensor.board_id} ({_format_can_id(s.can_id)} | {s.sensor.port}): "
            f"{len(s.generated)} generated, {len(s.received_by_any)} received{suffix}"
        )
        if s.total_misses:
            lines.append(f"   - MISSES ({len(s.total_misses)}): {_format_counter_list(s.total_misses)}")
    return "\n".join(lines), passed


def analyze_pair_delivery_info(sensors, receivers, context: AnalysisContext = None):
    results = compare(sensors, receivers, context)
    lines = ["PAIR DELIVERY INFO (informational only; does not affect pass/fail)"]
    visible = [r for r in results if r.sent or r.received or r.missed]
    if not visible:
        lines.append("  no subscribed sensor/receiver pairs with transport data")
        return "\n".join(lines)
    for r in visible:
        line = f"  Sensor {r.sensor.board_id} {_format_can_id(r.can_id)} -> Receiver {r.receiver.board_id}: {len(r.received)}/{len(r.sent)} received"
        if r.missed:
            line += f", missed {len(r.missed)} {_format_counter_list(r.missed)}"
        lines.append(line)
    return "\n".join(lines)


def analyze_crashes(sensors, receivers):
    incidents = []
    for device in sensors + receivers:
        for line, desc in device.crash_details:
            incidents.append(f"  {device.__class__.__name__} {device.board_id}: {desc} at line {line}")
    passed = not incidents
    return "\n".join([f"CRASH ANALYSIS: {'PASS' if passed else 'FAIL'} ({len(incidents)} total incidents)"] + incidents), passed


def analyze_measure(sensors, receivers):
    devices = sensors + receivers
    if not any(d.measure.values for d in devices):
        return "MEASURE: no final device measurements", True
    lines = ["MEASURE:"]
    for d in devices:
        if not d.measure.values:
            continue
        kind = "Sensor" if isinstance(d, SensorData) else "Receiver"
        rendered = " ".join(f"{k}={v}" for k, v in sorted(d.measure.values.items()))
        lines.append(f"  {kind} {d.board_id}: {rendered}")
    return "\n".join(lines), True


def analyze_batch_stats(sensors, context: AnalysisContext = None):
    context = context or AnalysisContext()
    devices = [s for s in sensors if s.batch_stats]
    if not devices:
        return "BATCH STATS: no final batch stats", True

    expected_points = None
    expected_hz = None
    metadata = context.metadata
    if metadata.get("frequency_hz") is not None and metadata.get("linger_ms") is not None:
        expected_points = float(metadata["frequency_hz"]) * float(metadata["linger_ms"]) / 1000.0
        expected_hz = 1000.0 / float(metadata["linger_ms"]) if float(metadata["linger_ms"]) > 0 else None

    lines = ["BATCH STATS:"]
    if expected_hz is not None and expected_points is not None:
        lines.append(f"  Expected: avg_hz~{expected_hz:.2f} avg_points~{expected_points:.1f}")

    ordered_keys = [
        "count", "avg_hz", "avg_points", "min_points", "max_points",
        "avg_interval_ms", "min_interval_ms", "max_interval_ms",
        "avg_dispatch_ms", "max_dispatch_ms",
    ]
    for sensor in devices:
        for can_id in sorted(sensor.batch_stats):
            values = sensor.batch_stats[can_id].values
            rendered = " ".join(f"{k}={values[k]}" for k in ordered_keys if k in values)
            notes = []
            if expected_hz is not None and "avg_hz" in values:
                avg_hz = float(values["avg_hz"])
                notes.append("rate=OK" if avg_hz >= expected_hz * 0.95 else "rate=SLOW")
            if expected_points is not None and "avg_points" in values:
                avg_points = float(values["avg_points"])
                if avg_points > expected_points * 1.5:
                    notes.append("points=LARGE")
                elif avg_points < expected_points * 0.5:
                    notes.append("points=SMALL")
                else:
                    notes.append("points=OK")
            suffix = f" ({', '.join(notes)})" if notes else ""
            lines.append(f"  Sensor {sensor.board_id} {_format_can_id(can_id)}: {rendered}{suffix}")
    return "\n".join(lines), True


def analyze_log_counters(sensors, receivers):
    devices = sensors + receivers
    total_p_fail = sum(d.p_fail_count for d in devices)
    total_p_full = sum(d.p_full_count for d in devices)
    total_s_fail = sum(d.s_fail_count for d in devices)
    lines = ["LOG COUNTERS:"]
    lines.append(f"  Total: P(FAIL)={total_p_fail} P(FULL)={total_p_full} S(FAIL)={total_s_fail}")
    for d in devices:
        kind = "Sensor" if isinstance(d, SensorData) else "Receiver"
        if d.p_fail_count or d.p_full_count or d.s_fail_count:
            lines.append(
                f"  {kind} {d.board_id}: "
                f"P(FAIL)={d.p_fail_count} P(FULL)={d.p_full_count} S(FAIL)={d.s_fail_count}"
            )
    if len(lines) == 2:
        lines.append("  No P(FAIL), P(FULL), or S(FAIL) entries found")
    return "\n".join(lines), True


def _indent_block(text: str, spaces: int = 4) -> str:
    prefix = " " * spaces
    return "\n".join(prefix + line if line else line for line in text.splitlines())


def _format_test_report(header: str, sections: list[str], failed_parts: list[str]) -> str:
    body = []
    if failed_parts:
        body.append("FAILED PARTS: " + ", ".join(failed_parts))
    body.extend(sections)
    return header + "\n" + _indent_block("\n\n".join(body))


def _receiver_observed_total(receiver: ReceiverData, allowed_can_ids=None) -> int:
    allowed = None if allowed_can_ids is None else {_normalize_can_id(c) for c in allowed_can_ids}
    return sum(len(counters) for can_id, counters in receiver.received.items() if allowed is None or can_id in allowed)


def analyze_scenario(test_dir: Path, sensors, receivers, context: AnalysisContext = None):
    context = context or AnalysisContext()
    metadata = context.metadata
    if not metadata:
        return "SCENARIO ANALYSIS: none", True
    scenario = metadata.get("scenario", "baseline")
    failures = 0
    lines = []

    if metadata.get("active_filtering"):
        lines.append("  Active filter delivery:")
        for can_id, expected in sorted(context.expected_receivers.items()):
            if not expected:
                lines.append(f"    {_format_can_id(can_id)} -> expected no receivers")
        for receiver in receivers:
            allowed = context.receiver_allowlists.get(receiver.board_id, set())
            unexpected = sorted(can_id for can_id, counters in receiver.received.items() if counters and can_id not in allowed)
            if unexpected:
                failures += len(unexpected)
                lines.append(f"    Receiver {receiver.board_id}: unexpected CAN IDs {', '.join(_format_can_id(i) for i in unexpected)}")
        for receiver in receivers:
            allowed = sorted(context.receiver_allowlists.get(receiver.board_id, set()))
            if not allowed:
                lines.append(f"    RECEIVER {receiver.board_id} -> expected none -> received {_receiver_observed_total(receiver)} total")
            for can_id in allowed:
                count = _receiver_observed_total(receiver, [can_id])
                lines.append(f"    RECEIVER {receiver.board_id} -> expected {_format_can_id(can_id)} -> received {count}")
                if count == 0:
                    failures += 1
        lines.append("  Active filtering checks complete")

    tolerance = float(metadata.get("frequency_tolerance", 0.05))
    expected_freqs = {bid: float(freq) for bid, freq in metadata.get("sensor_frequencies", {}).items()}
    global_freq = metadata.get("frequency_hz")
    for sensor in sensors:
        expected = expected_freqs.get(sensor.board_id) or (float(global_freq) if global_freq is not None else None)
        if expected is None:
            continue
        observed = sensor.avg_hz
        if observed is None:
            failures += 1
            lines.append(f"  Sensor {sensor.board_id}: no average frequency reported")
            continue
        low = expected * (1.0 - tolerance)
        high = expected * (1.0 + tolerance)
        ok = low <= observed <= high
        if not ok:
            failures += 1
        lines.append(f"  Sensor {sensor.board_id}: expected {expected:.1f}Hz, observed {observed:.1f}Hz ({'OK' if ok else 'FAIL'}, tolerance +/-{tolerance * 100:.0f}%)")

    passed = failures == 0
    return "\n".join([f"SCENARIO ANALYSIS: {'PASS' if passed else 'FAIL'} ({scenario}, failures={failures})"] + lines), passed


def is_test_folder(path: Path) -> bool:
    return any(path.glob("sensor_*.log")) and any(path.glob("receiver_*.log"))


def find_test_folders(root: Path) -> list[Path]:
    if is_test_folder(root):
        return [root]
    folders = sorted(p for p in root.rglob("test_*") if p.is_dir() and is_test_folder(p))
    if not folders:
        print(f"No test folders found in {root}")
        sys.exit(1)
    return folders


def _frequency_label(metadata: dict) -> str:
    if metadata.get("sensor_frequencies"):
        return ",".join(f"{bid}={int(freq)}Hz" for bid, freq in sorted(metadata["sensor_frequencies"].items()))
    if metadata.get("frequency_hz") is not None:
        return f"{int(metadata['frequency_hz'])}Hz"
    return "freq=unknown"


def _topology_and_repeat(test_dir: Path, metadata: dict):
    topology = str(metadata.get("topology", test_dir.name.removeprefix("test_")))
    repeat = metadata.get("repeat")
    return topology, (None if repeat is None else int(repeat))


def analysis_run_result(test_dir: Path, passed: bool) -> AnalysisRunResult:
    metadata = load_analysis_context(test_dir).metadata
    topology, repeat = _topology_and_repeat(test_dir, metadata)
    return AnalysisRunResult(test_dir, passed, metadata.get("suite", metadata.get("scenario", "unknown")), metadata.get("transport", "unknown"), _frequency_label(metadata), topology, repeat)


def format_topology_summary(results: list[AnalysisRunResult], min_successful_reps: int = 1) -> str:
    groups = {}
    for result in results:
        groups.setdefault((result.suite, result.transport, result.frequency, result.topology), []).append(result)
    lines = []
    passed_topologies = 0
    for key in sorted(groups):
        suite, transport, frequency, topology = key
        runs = groups[key]
        passed = [r for r in runs if r.passed]
        if len(passed) >= min_successful_reps:
            passed_topologies += 1
            status = "PASS"
        else:
            status = "FAIL"
        lines.append(f"{suite} | {transport} | {frequency} | {topology} | {status} ({len(passed)}/{len(runs)} reps passed)")
    header = f"TOPOLOGY RESULT: {passed_topologies}/{len(groups)} topologies passed (>={min_successful_reps} successful reps)"
    return header + ("\n" + "\n".join(lines) if lines else "")


def analyze_all(test_dir: Path):
    context = load_analysis_context(test_dir)
    metadata = context.metadata
    header_base = f"{metadata.get('suite', metadata.get('scenario', 'unknown'))} | {metadata.get('transport', 'unknown')} | {_frequency_label(metadata)} | {metadata.get('topology', test_dir.name)}"
    try:
        sensors = sorted((parse_sensor_log(p) for p in test_dir.glob("sensor_*.log")), key=lambda s: s.board_id)
        receivers = sorted((parse_receiver_log(p) for p in test_dir.glob("receiver_*.log")), key=lambda r: r.board_id)
    except ValueError as exc:
        header = f"{header_base} | FAIL"
        report = _format_test_report(header, ["INCOMPLETE LOGS: FAIL", f"  {exc}"], ["incomplete_logs"])
        return report, False
    if not sensors or not receivers:
        header = f"{header_base} | FAIL"
        return _format_test_report(header, [f"No logs found in {test_dir}"], ["logs"]), False

    r_delivery, p_delivery = analyze_delivery(sensors, receivers, test_dir.name, context)
    r_pair = analyze_pair_delivery_info(sensors, receivers, context)
    r_crash, p_crash = analyze_crashes(sensors, receivers)
    r_scenario, p_scenario = analyze_scenario(test_dir, sensors, receivers, context)
    r_measure, p_measure = analyze_measure(sensors, receivers)
    r_batch, _p_batch = analyze_batch_stats(sensors, context)
    r_log_counters, _p_log_counters = analyze_log_counters(sensors, receivers)
    checks = [
        ("delivery", p_delivery),
        ("crash", p_crash),
        ("scenario", p_scenario),
        ("measure", p_measure),
    ]
    failed_parts = [name for name, ok in checks if not ok]
    passed = not failed_parts
    status = "PASS" if passed else "FAIL"
    header = f"{header_base} | {status}"
    report = _format_test_report(header, [r_delivery, r_pair, r_crash, r_scenario, r_measure, r_batch, r_log_counters], failed_parts)
    return report, passed


def analyze_path(root: Path, write_files: bool = False):
    folders = find_test_folders(root)
    reports = []
    results = []
    passed = 0
    for folder in folders:
        report, ok = analyze_all(folder)
        reports.append(report)
        results.append(analysis_run_result(folder, ok))
        if write_files:
            (folder / "analysis.txt").write_text(report, encoding="utf-8")
        if ok:
            passed += 1
    combined = f"RESULT: {passed}/{len(folders)} tests passed\n{format_topology_summary(results)}\n\n======================\n\n" + "\n\n".join(reports)
    if write_files:
        if len(folders) == 1 and folders[0] == root:
            (root / "analysis.txt").write_text(reports[0], encoding="utf-8")
        else:
            (root / "analysis_summary.txt").write_text(combined, encoding="utf-8")
    return combined, passed == len(folders)


def main():
    parser = argparse.ArgumentParser(description="Analyze WCAN final-summary logs")
    parser.add_argument("path", type=Path, help="Test folder or results root")
    parser.add_argument("--output", type=Path, default=None, help="Optional output summary path")
    args = parser.parse_args()
    report, ok = analyze_path(args.path, write_files=args.output is None)
    if args.output:
        args.output.write_text(report, encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        if is_test_folder(args.path):
            print(f"Wrote {args.path / 'analysis.txt'}")
        else:
            print(f"Wrote {args.path / 'analysis_summary.txt'}")
            print("Wrote analysis.txt inside each test folder")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
