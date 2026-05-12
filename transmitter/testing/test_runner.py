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
import os
import random
import shutil
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
import yaml

from idf_env import check_idf
from build import build_needed, build_variant, VALID_TRANSPORTS
from flash import flash_all
from monitor import monitor_all_boards

VALID_SCENARIOS = ("baseline", "active_filtering", "frequency_mixing", "advanced")
FREQUENCY_TOLERANCE = 0.20


# ─── Config Loading ──────────────────────────────────────────────────────────

def load_config(config_path: str) -> dict:
    with open(config_path, "r") as f:
        cfg = yaml.safe_load(f)

    boards = cfg.get("boards", [])
    if len(boards) < 2:
        print("[ERROR] Need at least 2 boards in boards.yaml")
        sys.exit(1)

    for b in boards:
        if b["chip"] not in ("esp32", "esp32c3"):
            print(f"[ERROR] Board {b['id']} has invalid chip '{b['chip']}'. Must be 'esp32' or 'esp32c3'.")
            sys.exit(1)

    tc = cfg.get("test_config", {})
    config = {
        "boards": boards,
        "duration": tc.get("duration_seconds", 30),
        "repeats": tc.get("repeats", 3),
        "cooldown": tc.get("cooldown_seconds", 5),
        "baud": tc.get("baud_rate", 115200),
        "project_path": tc.get("project_path", "."),
        # transport + measure are populated from CLI args in main(); defaults match
        # the legacy behaviour so existing scripts keep working.
        "transport": "BROADCAST",
        "measure": False,
        "frequencies": tc.get("frequency_hz", [200]),
        "active_filtering_frequency": tc.get("active_filtering_frequency_hz", 200),
        "frequency_tolerance": tc.get("frequency_tolerance", FREQUENCY_TOLERANCE),
    }
    return config


# ─── Test Matrix ─────────────────────────────────────────────────────────────

def generate_test_matrix(n_boards: int) -> list:
    """
    Generate all (S, R) pairs where S >= 1, R >= 1, S + R <= N.
    Returns list of (sensor_count, receiver_count) tuples.
    """
    cases = []
    for s in range(1, n_boards):
        for r in range(1, n_boards - s + 1):
            cases.append((s, r))
    return cases


def print_test_matrix(cases: list, n_boards: int, repeats: int, duration: int, cooldown: int):
    """Print the test plan summary."""
    total_runs = len(cases) * repeats
    # Rough estimate: flash ~20s + duration + cooldown per run
    est_minutes = total_runs * (20 + duration + cooldown) / 60

    print(f"\n{'='*60}")
    print(f"  TEST MATRIX")
    print(f"{'='*60}")
    print(f"  Boards:       {n_boards}")
    print(f"  Test cases:   {len(cases)}")
    print(f"  Repeats:      {repeats}")
    print(f"  Total runs:   {total_runs}")
    print(f"  Est. time:    ~{est_minutes:.0f} minutes")
    print(f"{'='*60}")
    print(f"  {'Sensors':>8} : {'Receivers':<8}")
    print(f"  {'-'*20}")
    for s, r in cases:
        print(f"  {s:>8} : {r:<8}")
    print()


def normalize_can_id(can_id) -> int:
    if isinstance(can_id, str):
        return int(can_id.lower().removeprefix("0x"), 16)
    return int(can_id)


def board_can_id(board: dict) -> int:
    if "can_id" not in board:
        print(f"[ERROR] Board {board['id']} is missing can_id in boards.yaml; required for scenario tests.")
        sys.exit(1)
    return normalize_can_id(board["can_id"])


def format_can_ids(can_ids: list[int]) -> list[str]:
    return [f"0x{can_id:08x}" for can_id in can_ids]


def assignment_options(board: dict, role: str, scenario_state: dict) -> dict:
    board_id = board["id"]
    options = {}
    if role == "SENSOR":
        freq = scenario_state.get("sensor_frequencies", {}).get(board_id)
        if freq is not None:
            options["sensor_freq"] = int(freq)
    elif role == "RECEIVER" and "receiver_allowlists" in scenario_state:
        options["receiver_filter_ids"] = scenario_state["receiver_allowlists"].get(board_id, [])
    return options


def make_active_filtering_state(sensor_boards: list, receiver_boards: list, rng: random.Random) -> dict:
    sensor_ids = {b["id"]: board_can_id(b) for b in sensor_boards}
    receiver_allowlists = {b["id"]: [] for b in receiver_boards}
    expected_receivers = {f"0x{can_id:08x}": [] for can_id in sensor_ids.values()}

    if len(receiver_boards) == 1:
        receiver_id = receiver_boards[0]["id"]
        ids = list(sensor_ids.values())
        receiver_allowlists[receiver_id] = ids
        for can_id in ids:
            expected_receivers[f"0x{can_id:08x}"].append(receiver_id)
    else:
        shuffled_receivers = receiver_boards.copy()
        rng.shuffle(shuffled_receivers)
        for index, (_sensor_id, can_id) in enumerate(sensor_ids.items()):
            receiver = shuffled_receivers[index % len(shuffled_receivers)]
            receiver_allowlists[receiver["id"]].append(can_id)
            expected_receivers[f"0x{can_id:08x}"].append(receiver["id"])

    return {
        "active_filtering": True,
        "sensor_can_ids": {board_id: f"0x{can_id:08x}" for board_id, can_id in sensor_ids.items()},
        "receiver_allowlists": {
            board_id: format_can_ids(ids) for board_id, ids in receiver_allowlists.items()
        },
        "expected_receivers": expected_receivers,
    }


def make_frequency_mixing_state(sensor_boards: list, frequency_pool: list[int], rng: random.Random) -> dict:
    if len(sensor_boards) > len(frequency_pool):
        print(
            f"[ERROR] frequency_mixing needs {len(sensor_boards)} unique frequencies, "
            f"but boards.yaml only provides {len(frequency_pool)}."
        )
        sys.exit(1)
    sampled = rng.sample([int(f) for f in frequency_pool], len(sensor_boards))
    return {
        "frequency_mixing": True,
        "sensor_frequencies": {
            board["id"]: sampled[index] for index, board in enumerate(sensor_boards)
        },
    }


def build_scenario_state(sensor_boards: list, receiver_boards: list, config: dict, rng: random.Random) -> dict:
    scenario = config.get("scenario", "baseline")
    state = {
        "scenario": scenario,
        "seed": config.get("seed"),
        "transport": config["transport"],
        "frequency_tolerance": config.get("frequency_tolerance", FREQUENCY_TOLERANCE),
    }
    if scenario == "active_filtering":
        state.update(make_active_filtering_state(sensor_boards, receiver_boards, rng))
        if "current_freq" in config:
            state["sensor_frequencies"] = {board["id"]: int(config["current_freq"]) for board in sensor_boards}
    elif scenario == "frequency_mixing":
        state.update(make_frequency_mixing_state(sensor_boards, config["frequencies"], rng))
    return state


def write_scenario_metadata(log_dir: str, sensor_boards: list, receiver_boards: list,
                            idle_boards: list, scenario_state: dict):
    os.makedirs(log_dir, exist_ok=True)
    metadata = dict(scenario_state)
    metadata.update({
        "roles": {
            "sensors": [b["id"] for b in sensor_boards],
            "receivers": [b["id"] for b in receiver_boards],
            "idle": [b["id"] for b in idle_boards],
        },
        "boards": {
            b["id"]: {
                "chip": b["chip"],
                "port": b["port"],
                "can_id": f"0x{board_can_id(b):08x}" if "can_id" in b else None,
            }
            for b in sensor_boards + receiver_boards + idle_boards
        },
    })
    with open(os.path.join(log_dir, "scenario.json"), "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2, sort_keys=True)


def ensure_assignment_builds(assignments: list, config: dict) -> bool:
    seen = set()
    for assignment in assignments:
        if len(assignment) == 2:
            board, role = assignment
            options = {}
        else:
            board, role, options = assignment

        sensor_freq = int(options.get("sensor_freq", config.get("current_freq", 200)))
        receiver_filter_ids = options.get("receiver_filter_ids")
        key = (
            board["chip"], role, config["transport"], config["measure"],
            sensor_freq if role == "SENSOR" else 200,
            tuple(receiver_filter_ids) if receiver_filter_ids is not None else None,
        )
        if key in seen:
            continue
        seen.add(key)
        if not build_variant(
            board["chip"], role, config["transport"], config["measure"],
            config["project_path"], sensor_freq, receiver_filter_ids,
        ):
            return False
    return True


# ─── Test Execution ──────────────────────────────────────────────────────────

def run_single_test(
    sensor_boards: list,
    receiver_boards: list,
    idle_boards: list,
    config: dict,
    log_dir: str,
    scenario_state: dict,
) -> str:
    """
    Run one test case: flash, monitor, save logs.
    Idle boards are flashed with IDLE firmware (no WiFi, no WCAN — completely silent).
    Returns status string: "OK", "FLASH_FAIL", or "MONITOR_ERROR".
    """
    s_count = len(sensor_boards)
    r_count = len(receiver_boards)

    # Active boards: sensors + receivers (these get monitored)
    active_boards = [(b, "SENSOR") for b in sensor_boards] + [(b, "RECEIVER") for b in receiver_boards]
    # All boards to flash: active + idle (idle get IDLE firmware — completely off the network)
    all_to_flash = []
    for b, role in active_boards:
        options = assignment_options(b, role, scenario_state)
        if role == "SENSOR" and "sensor_freq" not in options:
            options["sensor_freq"] = int(config.get("current_freq", 200))
        all_to_flash.append((b, role, options))
    all_to_flash.extend((b, "IDLE", {}) for b in idle_boards)

    port_list = ", ".join(f"{b['id']}={b['port']}({role[0]})" for b, role in active_boards)
    idle_list = ", ".join(b["id"] for b in idle_boards)
    print(f"\n  [{s_count}S-{r_count}R] Active: {port_list}")
    if idle_boards:
        print(f"  Idle (silenced): {idle_list}")
    if scenario_state.get("sensor_frequencies"):
        freq_list = ", ".join(
            f"{board_id}={freq}Hz" for board_id, freq in sorted(scenario_state["sensor_frequencies"].items())
        )
        print(f"  Sensor frequencies: {freq_list}")
    if scenario_state.get("receiver_allowlists"):
        for board_id, ids in sorted(scenario_state["receiver_allowlists"].items()):
            print(f"  Receiver {board_id} allowlist: {', '.join(ids) if ids else '(empty)'}")

    write_scenario_metadata(log_dir, sensor_boards, receiver_boards, idle_boards, scenario_state)

    if config.get("scenario", "baseline") != "baseline" and not config.get("skip_build") and not config.get("dry_run"):
        print("  Building scenario-specific firmware variants...")
        if not ensure_assignment_builds(all_to_flash, config):
            return "BUILD_FAIL"

    # Flash ALL boards (active + idle)
    print(f"  Flashing {len(all_to_flash)} boards ({len(active_boards)} active + {len(idle_boards)} idle)...")
    if not flash_all(all_to_flash, config["transport"], config["measure"], config["project_path"]):
        return "FLASH_FAIL"

    # Brief pause after flashing before starting monitors
    time.sleep(1)

    # Monitor only active boards (not idle)
    ok = monitor_all_boards(active_boards, config["baud"], config["duration"], log_dir)

    return "OK" if ok else "MONITOR_ERROR"


def run_all_tests(config: dict, results_dir: str, global_run_state: dict, dry_run: bool = False, test_filter: set = None) -> int:
    """Run the complete test matrix. Returns the number of runs executed (0 on dry-run / empty filter)."""
    boards = config["boards"]
    n = len(boards)
    cases = generate_test_matrix(n)
    rng = config.get("rng", random)

    # Apply filter if specified
    if test_filter:
        filtered = [c for c in cases if c in test_filter]
        invalid = test_filter - set(cases)
        if invalid:
            for s, r in invalid:
                if s + r > n:
                    print(f"[WARNING] Test {s}S-{r}R requires {s+r} boards but only {n} available. Skipping.")
                else:
                    print(f"[WARNING] Test {s}S-{r}R is not in the matrix. Skipping.")
        cases = filtered
        if not cases:
            print("[ERROR] No valid test cases after filtering.")
            return 0
    repeats = config["repeats"]

    print_test_matrix(cases, n, repeats, config["duration"], config["cooldown"])

    if dry_run:
        print("[DRY RUN] Showing board assignments (actual runs will be shuffled):\n")
        for s, r in cases:
            print(f"  {s}S-{r}R: {s} random sensors, {r} random receivers, {n - s - r} idle")
        print("\n[DRY RUN] No tests executed.")
        return 0

    # Summary data
    summary_rows = []

    total_runs = len(cases) * repeats
    run_number = 0

    for s_count, r_count in cases:
        for rep in range(repeats):
            # Shuffle board order each repeat so different boards get different roles
            shuffled = boards.copy()
            rng.shuffle(shuffled)

            sensor_boards = shuffled[:s_count]
            receiver_boards = shuffled[s_count : s_count + r_count]
            idle_boards = shuffled[s_count + r_count :]
            scenario_state = build_scenario_state(sensor_boards, receiver_boards, config, rng)

            run_number += 1
            global_run_state["current"] += 1
            test_name = f"test_{s_count}S-{r_count}R"
            log_dir = os.path.join(results_dir, f"{test_name}_rep{rep}")

            print(f"\n{'─'*60}")
            freq_label = "mixed" if config.get("scenario") == "frequency_mixing" else f"{config.get('current_freq', 200)}Hz"
            print(
                f"  RUN {global_run_state['current']}/{global_run_state['total']}: "
                f"{test_name} rep={rep} | {config['transport'].lower()} | "
                f"scenario={config.get('scenario', 'baseline')} | {freq_label}"
            )
            print(f"{'─'*60}")

            status = run_single_test(
                sensor_boards, receiver_boards, idle_boards, config, log_dir, scenario_state
            )

            print(f"  Status: {status}")
            print(f"  Logs:   {log_dir}")

            summary_rows.append({
                "test_name": test_name,
                "scenario": config.get("scenario", "baseline"),
                "transport": config["transport"],
                "measure": "1" if config["measure"] else "0",
                "sensor_freq": config.get("current_freq", 200),
                "sensor_freqs": ";".join(
                    f"{bid}:{freq}" for bid, freq in sorted(scenario_state.get("sensor_frequencies", {}).items())
                ),
                "receiver_allowlists": ";".join(
                    f"{bid}:{','.join(ids)}" for bid, ids in sorted(scenario_state.get("receiver_allowlists", {}).items())
                ),
                "seed": config.get("seed", ""),
                "sensors": ";".join(b["id"] for b in sensor_boards),
                "receivers": ";".join(b["id"] for b in receiver_boards),
                "sensor_chips": ";".join(b["chip"] for b in sensor_boards),
                "receiver_chips": ";".join(b["chip"] for b in receiver_boards),
                "sensor_ports": ";".join(b["port"] for b in sensor_boards),
                "receiver_ports": ";".join(b["port"] for b in receiver_boards),
                "repeat": rep,
                "status": status,
                "log_dir": os.path.basename(log_dir),
            })

            # Cooldown between tests (skip after last)
            if run_number < total_runs:
                print(f"  Cooldown {config['cooldown']}s...")
                time.sleep(config["cooldown"])

    # Write summary
    write_summary(summary_rows, results_dir)
    return total_runs


# ─── Summary ─────────────────────────────────────────────────────────────────

def write_summary(rows: list, results_dir: str):
    """Write summary.csv with one row per test run."""
    summary_path = os.path.join(results_dir, "summary.csv")
    fieldnames = [
        "test_name", "scenario", "transport", "measure", "sensor_freq", "sensor_freqs",
        "receiver_allowlists", "seed",
        "sensors", "receivers",
        "sensor_chips", "receiver_chips",
        "sensor_ports", "receiver_ports",
        "repeat", "status", "log_dir",
    ]

    with open(summary_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\n  Summary written: {summary_path}")

    # Print quick stats
    ok = sum(1 for r in rows if r["status"] == "OK")
    fail = len(rows) - ok
    print(f"  Results: {ok} OK, {fail} failed")


# ─── Analysis ────────────────────────────────────────────────────────────────

def run_analysis(results_dir: str) -> tuple[int, int]:
    """Run analyze_all from analysis.py on every test folder under results_dir, suppressing its
    stdout. Returns (passed, total). Writes summary.txt at the root when there's more than one test."""
    import contextlib
    import io

    try:
        from analysis import analyze_all
    except ImportError as e:
        print(f"[ANALYZE] Could not import analysis: {e}")
        return 0, 0

    root = Path(results_dir)

    if any(root.glob("sensor_*.log")) and any(root.glob("receiver_*.log")):
        test_folders = [root]
    else:
        test_folders = sorted(
            p for p in root.iterdir()
            if p.is_dir()
            and any(p.glob("sensor_*.log"))
            and any(p.glob("receiver_*.log"))
        )

    if not test_folders:
        print(f"[ANALYZE] No test folders found in {root}")
        return 0, 0

    n_passed = 0
    all_reports = []

    with contextlib.redirect_stdout(io.StringIO()):
        for folder in test_folders:
            try:
                report, passed = analyze_all(folder)
                all_reports.append(report)
                if passed:
                    n_passed += 1
            except Exception as e:
                all_reports.append(f"=== {folder.name} ===\n[ANALYZE ERROR] {e}\n")

    total = len(test_folders)
    if total > 1:
        result_line = f"RESULT: {n_passed}/{total} tests passed"
        summary_path = root / "analysis_summary.txt"
        combined = "\n".join(all_reports) + "\n" + result_line + "\n"
        summary_path.write_text(combined, encoding="utf-8")

    return n_passed, total


# ─── Pipeline (one transport) ────────────────────────────────────────────────

def run_pipeline(args, base_config, transport, results_dir_name, test_filter, freq, scenario, global_run_state):
    """Run build + tests + optional analyze for a single transport.
    Returns (total_runs, results_dir or None, n_passed, n_total)."""
    config = dict(base_config)
    config["transport"] = transport
    config["measure"] = args.measure
    config["current_freq"] = freq
    config["scenario"] = scenario
    config["seed"] = args.seed
    config["rng"] = args.rng
    config["skip_build"] = args.skip_build
    config["dry_run"] = args.dry_run

    print(f"\n{'='*60}")
    freq_text = "mixed" if scenario == "frequency_mixing" else f"{freq}Hz"
    print(f"  Pipeline: scenario={scenario}  transport={transport}  measure={config['measure']}  freq={freq_text}")
    print(f"  Results:  results/{results_dir_name}")
    print(f"{'='*60}")

    # Build phase
    if scenario == "baseline" and not args.skip_build and not args.dry_run:
        if not build_needed(set(b["chip"] for b in config["boards"]),
                            transport, config["measure"], config["project_path"], freq):
            print(f"\n[ABORT] Build failed for transport={transport}.")
            return 0, None, 0, 0

    # Create results directory and snapshot config
    results_dir = os.path.join("results", results_dir_name)
    if not args.dry_run:
        os.makedirs(results_dir, exist_ok=True)
        shutil.copy2(args.config, os.path.join(results_dir, "boards.yaml"))

    total_runs = run_all_tests(config, results_dir, global_run_state, dry_run=args.dry_run, test_filter=test_filter)

    n_passed, n_total = 0, 0
    if total_runs > 0 and args.analyze:
        n_passed, n_total = run_analysis(results_dir)

    return total_runs, results_dir, n_passed, n_total


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="WCAN Automated Test Runner")
    parser.add_argument("--config", default="boards.yaml", help="Path to boards.yaml (default: boards.yaml)")
    parser.add_argument("--skip-build", action="store_true", help="Skip building, reuse existing firmware")
    parser.add_argument("--dry-run", action="store_true", help="Print test matrix and assignments without executing")
    parser.add_argument("--test", type=str, action="append", default=None,
                        help="Run only specific S:R tests, e.g. --test 1:4 --test 2:3. Can be repeated.")
    parser.add_argument("--analyze", action="store_true",
                        help="After all tests complete, run analysis on results. Implied by --aggregate.")
    parser.add_argument("--name", type=str, default=None,
                        help="Override the results folder name (always inside results/). "
                             "Defaults to the current timestamp. With --transport BOTH, "
                             "becomes the prefix: <name>_broadcast/ and <name>_unicast/.")
    parser.add_argument("--transport", default="BROADCAST",
                        choices=VALID_TRANSPORTS + ("BOTH",),
                        help="Transport variant. BOTH runs the full matrix once per variant "
                             "into separate folders (default: BROADCAST).")
    parser.add_argument("--measure", action="store_true",
                        help="Enable -DMEASURE=1 instrumentation in firmware builds and tag in summary.csv. "
                             "Pairs with sdkconfig.measure overlay.")
    parser.add_argument("--aggregate", action="store_true",
                        help="After both variants run, call aggregate_thesis_data into "
                             "results/<name>_comparison.csv. Implies --analyze and "
                             "requires --transport BOTH.")
    parser.add_argument("--scenario", default="baseline", choices=VALID_SCENARIOS,
                        help="Scenario layer to run. baseline preserves the existing matrix. "
                             "advanced runs active_filtering and frequency_mixing separately.")
    parser.add_argument("--seed", type=int, default=None,
                        help="Seed for reproducible scenario board/frequency/filter assignments.")
    args = parser.parse_args()

    # --aggregate implies --analyze
    if args.aggregate:
        args.analyze = True

    if args.aggregate and args.transport != "BOTH":
        print("[ERROR] --aggregate requires --transport BOTH (need both variants to compare).")
        sys.exit(1)

    if args.name is not None:
        if "/" in args.name or "\\" in args.name or ".." in args.name or not args.name.strip():
            print(f"[ERROR] --name must be a simple folder name (no path separators or '..'): '{args.name}'")
            sys.exit(1)

    # Load config
    config = load_config(args.config)
    boards = config["boards"]

    print(f"\n{'='*60}")
    print(f"  WCAN Automated Test Runner")
    print(f"{'='*60}")
    print(f"  Config:    {args.config}")
    print(f"  Boards:    {len(boards)}")
    for b in boards:
        print(f"    {b['id']:>4}  {b['chip']:<10}  {b['port']}")
    print(f"  Transport: {args.transport}")
    print(f"  Scenario:  {args.scenario}")
    print(f"  Seed:      {args.seed if args.seed is not None else '(random)'}")
    print(f"  Measure:   {args.measure}")
    print(f"  Analyze:   {args.analyze}")
    print(f"  Aggregate: {args.aggregate}")
    print(f"  Duration:  {config['duration']}s per test")
    print(f"  Repeats:   {config['repeats']}")
    print(f"  Cooldown:  {config['cooldown']}s")
    print()

    # Dry-runs do not build or flash, so they should work outside an ESP-IDF shell.
    if not args.dry_run:
        check_idf()
    args.rng = random.Random(args.seed)

    # Parse --test filters
    test_filter = None
    if args.test:
        test_filter = set()
        for t in args.test:
            try:
                s, r = t.split(":")
                test_filter.add((int(s), int(r)))
            except ValueError:
                print(f"[ERROR] Invalid --test format '{t}'. Use S:R, e.g. --test 1:4")
                sys.exit(1)

    # Decide transport list and per-variant results-dir names
    base_name = args.name if args.name else datetime.now().strftime("%Y-%m-%d_%H%M%S")

    if args.transport == "BOTH":
        transports = ["BROADCAST", "UNICAST"]
    else:
        transports = [args.transport]

    if args.scenario in ("active_filtering", "advanced"):
        for b in boards:
            board_can_id(b)

    if args.scenario == "advanced":
        scenarios = ["active_filtering", "frequency_mixing"]
    else:
        scenarios = [args.scenario]

    # Calculate global runs total
    cases = generate_test_matrix(len(boards))
    if test_filter:
        cases = [c for c in cases if c in test_filter]
    global_total = 0
    for scenario in scenarios:
        freq_count = len(config["frequencies"]) if scenario == "baseline" else 1
        global_total += len(transports) * freq_count * len(cases) * config["repeats"]
    global_run_state = {"current": 0, "total": global_total}

    # Run each transport's pipeline
    grand_runs = 0
    grand_passed = 0
    grand_total = 0
    results_dirs = []
    for scenario in scenarios:
        scenario_freqs = config["frequencies"] if scenario == "baseline" else [config["active_filtering_frequency"]]
        for t in transports:
            for freq in scenario_freqs:
                parts = [base_name]
                if args.scenario != "baseline":
                    parts.append(scenario)
                if scenario == "baseline" and len(config["frequencies"]) > 1:
                    parts.append(f"{freq}Hz")
                if args.transport == "BOTH":
                    parts.append(t.lower())
                freq_dir_name = "_".join(parts)

                runs, rdir, passed, total = run_pipeline(
                    args, config, t, freq_dir_name, test_filter, freq, scenario, global_run_state
                )
                grand_runs += runs
                grand_passed += passed
                grand_total += total
                if rdir is not None:
                    results_dirs.append(rdir)

    # Optional cross-variant aggregation
    if args.aggregate and not args.dry_run:
        if len(results_dirs) < 2:
            print("\n[WARN] --aggregate requested but only one variant produced results; skipping.")
        else:
            from pathlib import Path
            from aggregate_thesis_data import aggregate
            out_path = Path("results") / f"{base_name}_comparison.csv"
            print(f"\nAggregating {len(results_dirs)} variants into {out_path}...")
            n = aggregate([Path(d) for d in results_dirs], out_path)
            if n == 0:
                print("[WARN] No measurement data found across variants; CSV not written.")
            else:
                print(f"  Wrote {n} rows to {out_path}")

    if grand_runs > 0:
        print(f"\n{'='*60}")
        print(f"  ALL DONE — {grand_runs} runs across {len(results_dirs)} variant(s)")
        for rd in results_dirs:
            print(f"  Logs: {rd}")
        if args.analyze and grand_total > 0:
            print(f"  Analyze: {grand_passed}/{grand_total} tests passed")
        print(f"{'='*60}\n")


if __name__ == "__main__":
    main()
