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

Requirements:
    pip install pyserial pyyaml

Must be run from an ESP-IDF shell (idf.py must be on PATH).
"""

import argparse
import csv
import os
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import serial
import yaml


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
    }
    return config


# ─── Build ───────────────────────────────────────────────────────────────────

# Maps (chip, role) -> (build_dir, sdkconfig)
BUILD_VARIANTS = {
    ("esp32", "SENSOR"):     ("build_esp32_sensor",     "sdkconfig_esp32"),
    ("esp32", "RECEIVER"):   ("build_esp32_receiver",   "sdkconfig_esp32"),
    ("esp32", "IDLE"):       ("build_esp32_idle",       "sdkconfig_esp32"),
    ("esp32c3", "SENSOR"):   ("build_esp32c3_sensor",   "sdkconfig_esp32c3"),
    ("esp32c3", "RECEIVER"): ("build_esp32c3_receiver", "sdkconfig_esp32c3"),
    ("esp32c3", "IDLE"):     ("build_esp32c3_idle",     "sdkconfig_esp32c3"),
}


def get_build_dir(chip: str, role: str) -> str:
    return BUILD_VARIANTS[(chip, role)][0]


def get_sdkconfig(chip: str, role: str) -> str:
    return BUILD_VARIANTS[(chip, role)][1]


def check_idf():
    """Verify idf.py is available."""
    if shutil.which("idf.py") is None:
        print("[ERROR] idf.py not found on PATH.")
        print("        Run this script from an ESP-IDF terminal (source export.sh / export.bat).")
        sys.exit(1)
    print("[OK] idf.py found")


def build_variant(chip: str, role: str, project_path: str) -> bool:
    """Build a single firmware variant. Returns True on success."""
    build_dir = get_build_dir(chip, role)
    sdkconfig = get_sdkconfig(chip, role)

    print(f"\n{'='*60}")
    print(f"  BUILDING: {chip} / {role}")
    print(f"  Build dir: {build_dir}")
    print(f"  sdkconfig: {sdkconfig}")
    print(f"{'='*60}")

    cmd = [
        "idf.py",
        "-B", build_dir,
        "--define-cache-entry", f"SDKCONFIG={sdkconfig}",
        f"-DROLE={role}",
        "build",
    ]

    # Remove IDF_TARGET from env so idf.py picks the target from sdkconfig
    env = os.environ.copy()
    env.pop("IDF_TARGET", None)

    result = subprocess.run(cmd, cwd=project_path, shell=True, env=env)
    if result.returncode != 0:
        print(f"[FAIL] Build failed: {chip}/{role}")
        return False

    print(f"[OK] Build succeeded: {chip}/{role}")
    return True


def build_all(boards: list, project_path: str) -> bool:
    """Build only the variants needed for the boards we have."""
    chips_present = set(b["chip"] for b in boards)
    needed = set()
    for chip in chips_present:
        needed.add((chip, "SENSOR"))
        needed.add((chip, "RECEIVER"))
        needed.add((chip, "IDLE"))

    print(f"\n[BUILD] Chips present: {chips_present}")
    print(f"[BUILD] Variants to build: {len(needed)}")

    for chip, role in sorted(needed):
        if not build_variant(chip, role, project_path):
            return False

    print(f"\n[BUILD] All {len(needed)} variants built successfully.\n")
    return True


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


# ─── Flashing ────────────────────────────────────────────────────────────────

def flash_board(chip: str, role: str, port: str, project_path: str) -> bool:
    """Flash a pre-built firmware to a board. Returns True on success."""
    build_dir = get_build_dir(chip, role)
    sdkconfig = get_sdkconfig(chip, role)

    cmd = [
        "idf.py",
        "-B", build_dir,
        "--define-cache-entry", f"SDKCONFIG={sdkconfig}",
        "-p", port,
        "flash",
    ]

    # Remove IDF_TARGET from env so idf.py picks the target from sdkconfig
    env = os.environ.copy()
    env.pop("IDF_TARGET", None)

    result = subprocess.run(cmd, cwd=project_path, capture_output=True, text=True, shell=True, env=env)
    if result.returncode != 0:
        print(f"  [FAIL] Flash failed on {port} ({chip}/{role})")
        print(f"         stderr: {result.stderr[-200:]}")
        return False

    print(f"  [OK] Flashed {port} ({chip}/{role})")
    return True


def flash_all_boards(assignments: list, project_path: str) -> bool:
    """
    Flash all boards in parallel.
    assignments: list of (board_dict, role_str)
    Returns True if all succeeded.
    """
    results = {}
    threads = []

    def _flash(board, role):
        ok = flash_board(board["chip"], role, board["port"], project_path)
        results[board["id"]] = ok

    for board, role in assignments:
        t = threading.Thread(target=_flash, args=(board, role))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    return all(results.values())


# ─── Serial Monitoring ───────────────────────────────────────────────────────

def monitor_board(port: str, baud: int, duration: float, log_path: str, stop_event: threading.Event):
    """
    Open serial port, hard-reset the board, capture output to a log file.
    Runs until duration elapses or stop_event is set.
    """
    try:
        ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.1)  # let the port settle after opening

        # Flush any leftover data from previous session
        ser.reset_input_buffer()

        # Hard reset: ESP-IDF standard sequence
        # RTS controls EN (chip reset), DTR controls IO0 (boot mode)
        ser.dtr = False   # IO0 = HIGH (normal boot, not download mode)
        ser.rts = True    # EN = LOW  (hold chip in reset)
        time.sleep(0.2)
        ser.rts = False   # EN = HIGH (release reset — chip boots now)
        ser.dtr = False   # keep IO0 HIGH
        time.sleep(0.5)

        # Do NOT flush here — we want to capture everything from boot

        start = time.time()
        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            f.write(f"# Monitor started: {datetime.now().isoformat()}\n")
            f.write(f"# Port: {port}, Baud: {baud}, Duration: {duration}s\n\n")

            while not stop_event.is_set() and (time.time() - start) < duration:
                try:
                    line = ser.readline()
                    if line:
                        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                        decoded = line.decode("utf-8", errors="replace").rstrip()
                        f.write(f"[{timestamp}] {decoded}\n")
                        f.flush()
                except serial.SerialException as e:
                    f.write(f"[ERROR] Serial exception: {e}\n")
                    break

            f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")

        ser.close()
        return True

    except serial.SerialException as e:
        # Write error to log even if port couldn't open
        with open(log_path, "w") as f:
            f.write(f"# FAILED TO OPEN PORT: {port}\n# Error: {e}\n")
        print(f"  [FAIL] Could not open {port}: {e}")
        return False


def monitor_all_boards(boards_with_roles: list, baud: int, duration: float, log_dir: str) -> bool:
    """
    Start monitoring all boards in parallel threads.
    boards_with_roles: list of (board_dict, role_str)
    Returns True if all monitors succeeded.
    """
    os.makedirs(log_dir, exist_ok=True)
    stop_event = threading.Event()
    threads = []
    results = {}

    for board, role in boards_with_roles:
        log_filename = f"{role.lower()}_{board['id']}_{board['port'].replace('/', '_')}.log"
        log_path = os.path.join(log_dir, log_filename)

        def _monitor(p=board["port"], lp=log_path, bid=board["id"]):
            ok = monitor_board(p, baud, duration, lp, stop_event)
            results[bid] = ok

        t = threading.Thread(target=_monitor)
        threads.append(t)
        t.start()

    # Wait for all monitors to finish (they self-terminate after duration)
    for t in threads:
        t.join(timeout=duration + 10)

    stop_event.set()  # Signal any stragglers
    return all(results.values())


# ─── Test Execution ──────────────────────────────────────────────────────────

def run_single_test(
    sensor_boards: list,
    receiver_boards: list,
    idle_boards: list,
    config: dict,
    log_dir: str,
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
    all_to_flash = active_boards + [(b, "IDLE") for b in idle_boards]

    port_list = ", ".join(f"{b['id']}={b['port']}({role[0]})" for b, role in active_boards)
    idle_list = ", ".join(b["id"] for b in idle_boards)
    print(f"\n  [{s_count}S-{r_count}R] Active: {port_list}")
    if idle_boards:
        print(f"  Idle (silenced): {idle_list}")

    # Flash ALL boards (active + idle)
    print(f"  Flashing {len(all_to_flash)} boards ({len(active_boards)} active + {len(idle_boards)} idle)...")
    if not flash_all_boards(all_to_flash, config["project_path"]):
        return "FLASH_FAIL"

    # Brief pause after flashing before starting monitors
    time.sleep(1)

    # Monitor only active boards (not idle)
    print(f"  Monitoring for {config['duration']}s...")
    ok = monitor_all_boards(active_boards, config["baud"], config["duration"], log_dir)

    return "OK" if ok else "MONITOR_ERROR"


def run_all_tests(config: dict, results_dir: str, dry_run: bool = False):
    """Run the complete test matrix."""
    boards = config["boards"]
    n = len(boards)
    cases = generate_test_matrix(n)
    repeats = config["repeats"]

    print_test_matrix(cases, n, repeats, config["duration"], config["cooldown"])

    if dry_run:
        print("[DRY RUN] Showing board assignments:\n")
        for s, r in cases:
            sensors = boards[:s]
            receivers = boards[s : s + r]
            idle = boards[s + r :]
            s_ids = ", ".join(b["id"] for b in sensors)
            r_ids = ", ".join(b["id"] for b in receivers)
            i_ids = ", ".join(b["id"] for b in idle) if idle else "none"
            print(f"  {s}S-{r}R: sensors=[{s_ids}]  receivers=[{r_ids}]  idle=[{i_ids}]")
        print("\n[DRY RUN] No tests executed.")
        return

    # Summary data
    summary_rows = []

    total_runs = len(cases) * repeats
    run_number = 0

    for s_count, r_count in cases:
        sensor_boards = boards[:s_count]
        receiver_boards = boards[s_count : s_count + r_count]
        idle_boards = boards[s_count + r_count :]

        for rep in range(repeats):
            run_number += 1
            test_name = f"test_{s_count}S-{r_count}R"
            log_dir = os.path.join(results_dir, f"{test_name}_rep{rep}")

            print(f"\n{'─'*60}")
            print(f"  RUN {run_number}/{total_runs}: {test_name} rep={rep}")
            print(f"{'─'*60}")

            status = run_single_test(
                sensor_boards, receiver_boards, idle_boards, config, log_dir
            )

            print(f"  Status: {status}")
            print(f"  Logs:   {log_dir}")

            summary_rows.append({
                "test_name": test_name,
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
    print(f"\n{'='*60}")
    print(f"  ALL DONE — {total_runs} runs completed")
    print(f"  Results: {results_dir}")
    print(f"{'='*60}\n")


# ─── Summary ─────────────────────────────────────────────────────────────────

def write_summary(rows: list, results_dir: str):
    """Write summary.csv with one row per test run."""
    summary_path = os.path.join(results_dir, "summary.csv")
    fieldnames = [
        "test_name", "sensors", "receivers",
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


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="WCAN Automated Test Runner")
    parser.add_argument("--config", default="boards.yaml", help="Path to boards.yaml (default: boards.yaml)")
    parser.add_argument("--skip-build", action="store_true", help="Skip building, reuse existing firmware")
    parser.add_argument("--dry-run", action="store_true", help="Print test matrix and assignments without executing")
    args = parser.parse_args()

    # Load config
    config = load_config(args.config)
    boards = config["boards"]

    print(f"\n{'='*60}")
    print(f"  WCAN Automated Test Runner")
    print(f"{'='*60}")
    print(f"  Config:  {args.config}")
    print(f"  Boards:  {len(boards)}")
    for b in boards:
        print(f"    {b['id']:>4}  {b['chip']:<10}  {b['port']}")
    print(f"  Duration:  {config['duration']}s per test")
    print(f"  Repeats:   {config['repeats']}")
    print(f"  Cooldown:  {config['cooldown']}s")
    print()

    # Check idf.py is available
    check_idf()

    # Build phase
    if not args.skip_build and not args.dry_run:
        if not build_all(boards, config["project_path"]):
            print("\n[ABORT] Build failed. Fix errors and retry.")
            sys.exit(1)

    # Create results directory
    timestamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    results_dir = os.path.join("results", timestamp)
    if not args.dry_run:
        os.makedirs(results_dir, exist_ok=True)
        # Copy config for reproducibility
        config_copy = os.path.join(results_dir, "boards.yaml")
        import shutil as _shutil
        _shutil.copy2(args.config, config_copy)

    # Run tests
    run_all_tests(config, results_dir, dry_run=args.dry_run)


if __name__ == "__main__":
    main()