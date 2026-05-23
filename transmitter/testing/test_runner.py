#!/usr/bin/env python3
"""
WCAN Automated Test Runner

Builds firmware variants, generates test matrix, flashes boards,
captures serial output, and saves logs for all (S, R) combinations.

Usage:
    python test_runner.py                    # Full run: build + test
    python test_runner.py --skip-build       # Skip build, reuse existing firmware
    python test_runner.py --dry-run          # Print test matrix without executing
    python test_runner.py --config my.yaml   # Use a custom config file
    python test_runner.py --analyze          # Run analyze_logs after the run, show pass/fail count
    python test_runner.py --name my_run      # Save logs to results/my_run instead of timestamp

Requirements:
    pip install pyserial pyyaml

Must be run from an ESP-IDF shell (idf.py must be on PATH).
"""

import argparse
import csv
import json
import shutil
import sys
import time
from datetime import datetime
from pathlib import Path

from idf_env import check_idf
from build import build_variant, VALID_TRANSPORTS
from flash import flash_all
from monitor import boot_config_line, monitor_all_boards, idle_all_boards
from wcan_test_config import board_can_ids, format_can_id, load_boards, load_tests
from wcan_test_plan import (
    build_run_plan,
    compact_build_dict,
    compact_run_dict,
    parse_test_filter,
    RunSettings,
)

VALID_SCENARIOS = ("baseline", "active_filtering", "frequency_mixing", "advanced")
SCENARIO_TO_SUITE = {
    "baseline": "baseline",
    "active_filtering": "active_filter",
    "frequency_mixing": "mixed_frequency",
    "advanced": "all",
}

_FLASHED_FIRMWARE = {}


def _assignment_board(assignment):
    if isinstance(assignment, dict):
        return assignment
    return assignment[0]


def _firmware_signature(board: dict, transport: str, measure: bool) -> tuple:
    return (board.get("chip", "esp32"), transport.upper(), bool(measure))


def flash_assignments_if_needed(assignments: list, transport: str, measure: bool,
                                project_path: str, flashed_state: dict | None = None) -> bool:
    flashed_state = _FLASHED_FIRMWARE if flashed_state is None else flashed_state
    to_flash = []
    for assignment in assignments:
        board = _assignment_board(assignment)
        desired = _firmware_signature(board, transport, measure)
        if flashed_state.get(board["id"]) != desired:
            to_flash.append(assignment)

    if not to_flash:
        print("  [FLASH] Reusing flashed firmware for all boards.")
        return True

    print(f"  [FLASH] Flashing {len(to_flash)}/{len(assignments)} boards for {transport.upper()} firmware...")
    if not flash_all(to_flash, transport, measure, project_path):
        return False

    for assignment in to_flash:
        board = _assignment_board(assignment)
        flashed_state[board["id"]] = _firmware_signature(board, transport, measure)
    return True


# ─── Plan-based Runner (current entrypoint) ─────────────────────────────────

def _safe_results_name(name: str | None) -> str:
    if name is None:
        return datetime.now().strftime("%Y-%m-%d_%H%M%S")
    if "/" in name or "\\" in name or ".." in name or not name.strip():
        print(f"[ERROR] --name must be a simple folder name: {name!r}")
        sys.exit(1)
    return name


def _format_filter(ids) -> str:
    if ids is None:
        return "accept-all"
    if not ids:
        return "empty"
    return ",".join(format_can_id(can_id) for can_id in ids)


def _run_dir(root: Path, run) -> Path:
    return root / run.transport.lower() / run.suite / run.frequency_label / run.test_folder_name


def _print_plan(plan, root: Path, dry_run: bool):
    print(f"\n{'=' * 72}")
    print("  WCAN Automated Test Runner")
    print(f"{'=' * 72}")
    print(f"  Results:      {root}")
    print(f"  Runs:         {len(plan.runs)}")
    print(f"  Build specs:  {len(plan.build_specs)}")
    print(f"  Transports:   {', '.join(plan.settings.transports)}")
    print(f"  Test duration: {plan.settings.test_duration_ms}ms")
    print(f"  Host wait:     {plan.settings.host_wait_time_ms}ms")
    print(f"  Repeats:      {plan.settings.repeats}")
    print(f"  Cooldown:     {plan.settings.cooldown}s")
    print(f"  Measure:      {plan.settings.measure}")
    print(f"  Seed:         {plan.settings.seed if plan.settings.seed is not None else '(random)'}")
    print(f"  Mode:         {'DRY RUN' if dry_run else 'EXECUTE'}")
    print(f"{'=' * 72}")

    print("\nBoards:")
    for board in plan.boards:
        print(f"  {board['id']}  {board['chip']:<7}  {board['port']:<14}  base_can_id={format_can_id(board['can_id'])}")

    print("\nRun matrix:")
    for run in plan.runs:
        sensors = ",".join(board["id"] for board in run.sensor_boards)
        receivers = ",".join(board["id"] for board in run.receiver_boards)
        freq = run.frequency_label
        if run.sensor_frequencies:
            freq = ", ".join(f"{bid}={hz}Hz" for bid, hz in sorted(run.sensor_frequencies.items()))
        print(
            f"  #{run.index:03d} {run.suite:<15} {run.transport:<9} {run.topology_label:<6} "
            f"rep={run.repeat} freq={freq} linger={run.linger_ms}ms ids/sensor={run.can_ids_per_sensor} "
            f"S=[{sensors}] R=[{receivers}]"
        )

    if dry_run:
        print("\nUnique firmware builds:")
        for spec in plan.build_specs:
            print(
                f"  {spec.chip:<7} runtime  {spec.transport:<9} measure={spec.measure}"
            )


def _write_plan_files(root: Path, boards_path: Path, tests_path: Path, plan):
    root.mkdir(parents=True, exist_ok=True)
    shutil.copy2(boards_path, root / "boards.yaml")
    shutil.copy2(tests_path, root / "tests.yaml")
    payload = {
        "settings": {
            "duration_seconds": plan.settings.duration,
            "test_duration_ms": plan.settings.test_duration_ms,
            "host_wait_time_ms": plan.settings.host_wait_time_ms,
            "repeats": plan.settings.repeats,
            "cooldown_seconds": plan.settings.cooldown,
            "baud_rate": plan.settings.baud,
            "project_path": plan.settings.project_path,
            "transports": plan.settings.transports,
            "measure": plan.settings.measure,
            "seed": plan.settings.seed,
        },
        "builds": [compact_build_dict(spec) for spec in plan.build_specs],
        "runs": [compact_run_dict(run) for run in plan.runs],
    }
    (root / "run_plan.json").write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def _build_plan_variants(plan, skip_build: bool, dry_run: bool) -> bool:
    if dry_run:
        print("\n[BUILD] Dry-run only.")
        return True
    if skip_build:
        print("\n[BUILD] Skipped, reusing existing firmware.")
        return True

    from tqdm import tqdm

    print("\n[BUILD] Building unique firmware variants...")
    for spec in tqdm(plan.build_specs, desc="  Builds", unit="build"):
        ok = build_variant(
            spec.chip,
            spec.transport,
            spec.measure,
            plan.settings.project_path,
            quiet=True,
        )
        if not ok:
            return False
    return True


def _assignment_options(run, board: dict, role: str) -> dict:
    if role == "SENSOR":
        return {
            "sensor_freq": run.sensor_frequencies.get(board["id"], run.frequency_hz or 200),
            "sensor_base_can_id": board["can_id"],
            "sensor_can_id_count": run.can_ids_per_sensor,
            "linger_ms": run.linger_ms,
        }
    if role == "RECEIVER" and board["id"] in run.receiver_allowlists:
        return {"receiver_filter_ids": run.receiver_allowlists[board["id"]]}
    return {}


def _write_scenario_metadata(log_dir: Path, run, settings: RunSettings):
    all_boards = run.sensor_boards + run.receiver_boards + run.idle_boards
    runtime_assignments = []
    runtime_assignments.extend(
        (board, "SENSOR", _assignment_options(run, board, "SENSOR")) for board in run.sensor_boards
    )
    runtime_assignments.extend(
        (board, "RECEIVER", _assignment_options(run, board, "RECEIVER")) for board in run.receiver_boards
    )
    runtime_assignments.extend((board, "IDLE", {}) for board in run.idle_boards)
    metadata = {
        "suite": run.suite,
        "scenario": run.suite,
        "profile": run.profile,
        "transport": run.transport,
        "topology": run.topology_label,
        "repeat": run.repeat,
        "frequency_hz": run.frequency_hz,
        "sensor_frequencies": run.sensor_frequencies,
        "frequency_tolerance": settings.frequency_tolerance,
        "test_duration_ms": settings.test_duration_ms,
        "host_wait_time_ms": settings.host_wait_time_ms,
        "linger_ms": run.linger_ms,
        "can_ids_per_sensor": run.can_ids_per_sensor,
        "roles": {
            "sensors": [board["id"] for board in run.sensor_boards],
            "receivers": [board["id"] for board in run.receiver_boards],
            "idle": [board["id"] for board in run.idle_boards],
        },
        "boards": {
            board["id"]: {
                "chip": board["chip"],
                "port": board["port"],
                "can_id": format_can_id(board["can_id"]),
                "can_ids": [format_can_id(can_id) for can_id in board_can_ids(board, run.can_ids_per_sensor)],
            }
            for board in all_boards
        },
        "uart_boot_config": {
            board["id"]: boot_config_line(
                role,
                {**options, "test_duration_ms": settings.test_duration_ms, "host_wait_time_ms": settings.host_wait_time_ms},
                run.transport,
            )
            for board, role, options in runtime_assignments
        },
    }
    if run.suite == "active_filter":
        metadata["active_filtering"] = True
        metadata["sensor_can_ids"] = {
            board["id"]: format_can_id(board["can_id"])
            for board in run.sensor_boards
        }
        metadata["receiver_allowlists"] = {
            board_id: [format_can_id(can_id) for can_id in ids]
            for board_id, ids in sorted(run.receiver_allowlists.items())
        }
        metadata["expected_receivers"] = run.expected_receivers
    if run.suite == "mixed_frequency":
        metadata["frequency_mixing"] = True

    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / "scenario.json").write_text(json.dumps(metadata, indent=2, sort_keys=True), encoding="utf-8")


def _summary_row(root: Path, log_dir: Path, run, status: str) -> dict:
    return {
        "suite": run.suite,
        "profile": run.profile,
        "transport": run.transport,
        "topology": run.topology_label,
        "repeat": run.repeat,
        "frequency_hz": run.frequency_hz if run.frequency_hz is not None else "mixed",
        "sensor_frequencies": ";".join(f"{bid}:{hz}" for bid, hz in sorted(run.sensor_frequencies.items())),
        "linger_ms": run.linger_ms,
        "can_ids_per_sensor": run.can_ids_per_sensor,
        "sensors": ";".join(board["id"] for board in run.sensor_boards),
        "receivers": ";".join(board["id"] for board in run.receiver_boards),
        "idle": ";".join(board["id"] for board in run.idle_boards),
        "receiver_allowlists": ";".join(
            f"{bid}:{','.join(format_can_id(can_id) for can_id in ids)}"
            for bid, ids in sorted(run.receiver_allowlists.items())
        ),
        "status": status,
        "log_dir": str(log_dir.relative_to(root)),
    }


def _run_one_plan_test(root: Path, plan, run, measure: bool, flashed_state: dict | None = None) -> dict:
    log_dir = _run_dir(root, run)
    _write_scenario_metadata(log_dir, run, plan.settings)

    assignments = []
    assignments.extend(
        (board, "SENSOR", _assignment_options(run, board, "SENSOR"))
        for board in run.sensor_boards
    )
    assignments.extend(
        (board, "RECEIVER", _assignment_options(run, board, "RECEIVER"))
        for board in run.receiver_boards
    )
    assignments.extend((board, "IDLE", {}) for board in run.idle_boards)

    print(f"\n{'-' * 72}")
    print(
        f"[RUN {run.index}/{len(plan.runs)}] suite={run.suite} transport={run.transport} "
        f"topology={run.topology_label} rep={run.repeat} freq={run.frequency_label} "
        f"linger={run.linger_ms}ms ids/sensor={run.can_ids_per_sensor}"
    )
    print(f"  Sensors:   {', '.join(board['id'] for board in run.sensor_boards)}")
    print(f"  Receivers: {', '.join(board['id'] for board in run.receiver_boards)}")
    if run.idle_boards:
        print(f"  Idle:      {', '.join(board['id'] for board in run.idle_boards)}")
    if run.sensor_frequencies:
        print("  Sensor frequencies: " + ", ".join(f"{bid}={hz}Hz" for bid, hz in sorted(run.sensor_frequencies.items())))
    if run.receiver_allowlists:
        for board_id, ids in sorted(run.receiver_allowlists.items()):
            print(f"  Receiver {board_id} allowlist: {', '.join(format_can_id(can_id) for can_id in ids) or '(empty)'}")
    print(f"  Logs: {log_dir}")

    if not flash_assignments_if_needed(assignments, run.transport, measure, plan.settings.project_path, flashed_state):
        status = "FLASH_FAIL"
    else:
        time.sleep(1)
        try:
            print(
                f"  [MONITOR] Sending UART config and waiting "
                f"{plan.settings.test_duration_ms + plan.settings.host_wait_time_ms}ms after start at {plan.settings.baud} baud..."
            )
            status = "OK" if monitor_all_boards(
                assignments,
                plan.settings.baud,
                plan.settings.test_duration_ms,
                plan.settings.host_wait_time_ms,
                str(log_dir),
                run.transport,
            ) else "MONITOR_ERROR"
        finally:
            idle_all_boards(
                assignments,
                plan.settings.baud,
                run.transport,
                plan.settings.test_duration_ms,
                plan.settings.host_wait_time_ms,
            )

    print(f"  [RESULT] {status}")
    return _summary_row(root, log_dir, run, status)


def _write_plan_summary(root: Path, rows: list[dict]):
    path = root / "summary.csv"
    fieldnames = [
        "suite", "profile", "transport", "topology", "repeat", "frequency_hz",
        "sensor_frequencies", "linger_ms", "can_ids_per_sensor", "sensors",
        "receivers", "idle", "receiver_allowlists", "status", "log_dir",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    ok = sum(1 for row in rows if row["status"] == "OK")
    print(f"\n[SUMMARY] {path} ({ok} OK, {len(rows) - ok} failed)")


def _find_plan_test_folders(root: Path) -> list[Path]:
    if any(root.glob("sensor_*.log")) and any(root.glob("receiver_*.log")):
        return [root]
    return sorted(
        path for path in root.rglob("test_*")
        if path.is_dir() and any(path.glob("sensor_*.log")) and any(path.glob("receiver_*.log"))
    )


def _run_plan_analysis(root: Path) -> tuple[int, int]:
    import contextlib
    import io

    try:
        from analysis import analyze_all, analysis_run_result, format_topology_summary
    except ImportError as exc:
        print(f"[ANALYZE] Could not import analysis.py: {exc}")
        return 0, 0

    folders = _find_plan_test_folders(root)
    if not folders:
        print(f"[ANALYZE] No test folders found under {root}")
        return 0, 0

    passed = 0
    reports = []
    run_results = []
    with contextlib.redirect_stdout(io.StringIO()):
        for folder in folders:
            try:
                report, ok = analyze_all(folder)
            except Exception as exc:
                report, ok = f"{folder.name} - FAIL\nfailed: analyze_error\n[ANALYZE ERROR] {exc}\n", False
            reports.append(report)
            try:
                run_results.append(analysis_run_result(folder, ok))
            except Exception:
                pass
            if ok:
                passed += 1

    summary = root / "analysis_summary.txt"
    result_line = f"RESULT: {passed}/{len(folders)} tests passed"
    topology_summary = format_topology_summary(run_results)
    summary.write_text(
        f"{result_line}\n{topology_summary}\n\n" + "======================\n\n" + "\n\n".join(reports),
        encoding="utf-8",
    )
    print(f"[ANALYZE] {passed}/{len(folders)} tests passed; wrote {summary}")
    return passed, len(folders)


def plan_main(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description="WCAN Automated Test Runner")
    parser.add_argument("--boards", "--config", dest="boards", default="boards.yaml",
                        help="Path to boards.yaml hardware inventory.")
    parser.add_argument("--tests", default="tests.yaml",
                        help="Path to tests.yaml suite/profile configuration.")
    parser.add_argument("--suite", default="all",
                        help="Suite or suite group: baseline, multiple, real_time, active_filter, mixed_frequency, all.")
    parser.add_argument("--profile", default="fast",
                        help="Profile from tests.yaml (default: fast).")
    parser.add_argument("--scenario", choices=VALID_SCENARIOS, default=None,
                        help="Alias for --suite.")
    parser.add_argument("--transport", default=None, choices=VALID_TRANSPORTS + ("BOTH",),
                        help="Override tests.yaml transports.")
    parser.add_argument("--test", action="append", default=None,
                        help="Run only a specific topology, e.g. --test 4:1. Can be repeated.")
    parser.add_argument("--skip-build", action="store_true", help="Skip building, reuse existing firmware.")
    parser.add_argument("--dry-run", action="store_true", help="Print the run plan without building/flashing.")
    parser.add_argument("--analyze", "--analysis", dest="analyze", action="store_true", help="Run analysis.py after execution.")
    parser.add_argument("--measure", action="store_true", default=None,
                        help="Override tests.yaml and enable measurement-instrumented firmware builds.")
    parser.add_argument("--no-measure", action="store_false", dest="measure",
                        help="Override tests.yaml and disable measurement-instrumented firmware builds.")
    parser.add_argument("--aggregate", action="store_true", help="Aggregate measurement CSV after analysis.")
    parser.add_argument("--name", default=None, help="Results folder name under results/.")
    parser.add_argument("--seed", type=int, default=None, help="Seed for reproducible assignments.")
    args = parser.parse_args(argv)

    if args.scenario is not None:
        args.suite = SCENARIO_TO_SUITE[args.scenario]
    if args.aggregate:
        args.analyze = True

    boards_path = Path(args.boards)
    tests_path = Path(args.tests)
    boards = load_boards(boards_path)
    tests_cfg = load_tests(tests_path)
    test_filter = parse_test_filter(args.test, len(boards))
    plan = build_run_plan(
        boards=boards,
        tests_cfg=tests_cfg,
        suite_or_group=args.suite,
        profile_name=args.profile,
        cli_transport=args.transport,
        test_filter=test_filter,
        measure=args.measure,
        seed=args.seed,
    )
    if not plan.runs:
        print("[ERROR] Run plan is empty.")
        sys.exit(1)

    root = Path("results") / _safe_results_name(args.name)
    _print_plan(plan, root, args.dry_run)
    if args.dry_run:
        return

    check_idf()
    _write_plan_files(root, boards_path, tests_path, plan)
    if not _build_plan_variants(plan, args.skip_build, args.dry_run):
        print("[ABORT] Build failed.")
        sys.exit(1)

    rows = []
    flashed_state = {}
    for run in plan.runs:
        rows.append(_run_one_plan_test(root, plan, run, plan.settings.measure, flashed_state))
        if run.index < len(plan.runs):
            print(f"[COOLDOWN] {plan.settings.cooldown}s")
            time.sleep(plan.settings.cooldown)
    _write_plan_summary(root, rows)

    if args.analyze:
        _run_plan_analysis(root)

    if args.aggregate:
        from aggregate_thesis_data import aggregate
        out_path = root / "comparison.csv"
        variant_dirs = sorted({folder.parent for folder in _find_plan_test_folders(root)})
        count = aggregate(variant_dirs, out_path)
        if count:
            print(f"[AGGREGATE] Wrote {count} rows to {out_path}")
        else:
            print("[AGGREGATE] No measurement data found.")


if __name__ == "__main__":
    plan_main()
