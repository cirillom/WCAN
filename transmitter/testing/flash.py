#!/usr/bin/env python3
import argparse
import subprocess
import threading
import sys
import yaml
from idf_env import check_idf, get_idf_env
from build import get_build_dir, get_sdkconfig, VALID_TRANSPORTS

def flash_board(chip: str, role: str, port: str, transport: str = "BROADCAST",
                measure: bool = False, project_path: str = "..", sensor_freq: int = 200,
                receiver_filter_ids=None, sensor_base_can_id: int = 0x100,
                sensor_can_id_count: int = 1, linger_ms: int = 100) -> bool:
    """Flash a pre-built firmware to a board. Returns True on success."""
    role = role.upper()
    effective_measure = measure
    build_dir = get_build_dir(
        chip, role, transport, effective_measure, sensor_freq, receiver_filter_ids,
        sensor_base_can_id, sensor_can_id_count, linger_ms,
    )
    sdkconfig = get_sdkconfig(chip, effective_measure)

    cmd = [
        "idf.py",
        "-B", build_dir,
        f"-DSDKCONFIG={sdkconfig}",
        "-p", port,
        "flash",
    ]

    env = get_idf_env()
    result = subprocess.run(cmd, cwd=project_path, capture_output=True, text=True, shell=False, env=env)

    if result.returncode != 0:
        print(
            f"  [FAIL] Flash failed on {port} ({chip}/runtime/{transport}{'+measure' if effective_measure else ''}, "
            f"boot-role={role})"
        )
        print(f"         stderr: {result.stderr[-200:]}")
        return False

    print(
        f"  [OK] Flashed {port} ({chip}/runtime/{transport}{'+measure' if effective_measure else ''}, "
        f"boot-role={role})"
    )
    return True

def flash_all(assignments: list, transport: str = "BROADCAST", measure: bool = False,
              project_path: str = "..") -> bool:
    """
    Flash multiple boards in parallel.
    assignments: list of (board_dict, role_str) or (board_dict, role_str, options_dict)
    """
    results = {}
    threads = []

    def _flash(board, role, options):
        ok = flash_board(
            board["chip"], role, board["port"], transport, measure, project_path,
            options.get("sensor_freq", 200), options.get("receiver_filter_ids"),
            options.get("sensor_base_can_id", 0x100), options.get("sensor_can_id_count", 1),
            options.get("linger_ms", 100),
        )
        results[board["id"]] = ok

    for assignment in assignments:
        if len(assignment) == 2:
            board, role = assignment
            options = {}
        else:
            board, role, options = assignment
        t = threading.Thread(target=_flash, args=(board, role, options))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    return all(results.values())

def main():
    parser = argparse.ArgumentParser(description="WCAN Flash Script")
    parser.add_argument("--config", default="boards.yaml", help="Path to boards.yaml")
    parser.add_argument("--board", help="Board ID from boards.yaml to flash")
    parser.add_argument("--port", help="Serial port to flash")
    parser.add_argument("--chip", choices=["esp32", "esp32c3"], help="Chip type")
    parser.add_argument("--role", choices=["sensor", "receiver", "idle", "SENSOR", "RECEIVER", "IDLE"], help="Firmware role")
    parser.add_argument("--transport", default="BROADCAST", choices=VALID_TRANSPORTS,
                        help="Transport variant of the firmware to flash (default: BROADCAST). Ignored for IDLE.")
    parser.add_argument("--measure", action="store_true",
                        help="Flash measurement-instrumented binary (must have been built with --measure).")
    parser.add_argument("--project-path", default="..", help="Path to ESP-IDF project root")
    parser.add_argument("--sensor-freq", type=int, default=200,
                        help="Sensor sample frequency in Hz for SENSOR builds.")
    parser.add_argument("--sensor-base-can-id", default="0x100",
                        help="Base CAN ID for SENSOR builds.")
    parser.add_argument("--sensor-can-id-count", type=int, default=1,
                        help="Number of consecutive CAN IDs emitted by SENSOR builds.")
    parser.add_argument("--linger-ms", type=int, default=100,
                        help="WCAN linger window in milliseconds for SENSOR builds.")
    parser.add_argument("--receiver-filter-id", action="append", default=None,
                        help="CAN ID allowed by RECEIVER firmware. Repeat for multiple IDs.")

    args = parser.parse_args()
    check_idf()

    if args.port and args.chip and args.role:
        success = flash_board(args.chip, args.role, args.port, args.transport, args.measure, args.project_path,
                              args.sensor_freq, args.receiver_filter_id,
                              int(str(args.sensor_base_can_id).lower().removeprefix("0x"), 16),
                              args.sensor_can_id_count, args.linger_ms)
        sys.exit(0 if success else 1)

    if args.board or (not args.port and not args.chip and not args.role):
        with open(args.config, "r") as f:
            cfg = yaml.safe_load(f)
        boards = cfg.get("boards", [])

        if args.board:
            board = next((b for b in boards if b["id"] == args.board), None)
            if not board:
                print(f"[ERROR] Board {args.board} not found in {args.config}")
                sys.exit(1)

            if not args.role:
                print("[ERROR] Must specify --role when flashing by --board")
                sys.exit(1)

            success = flash_board(board["chip"], args.role, board["port"],
                                  args.transport, args.measure, args.project_path,
                                  args.sensor_freq, args.receiver_filter_id,
                                  int(str(args.sensor_base_can_id).lower().removeprefix("0x"), 16),
                                  args.sensor_can_id_count, args.linger_ms)
            sys.exit(0 if success else 1)
        else:
            print("[ERROR] Please provide --board and --role, or --port, --chip, and --role")
            sys.exit(1)

if __name__ == "__main__":
    main()
