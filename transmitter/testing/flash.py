#!/usr/bin/env python3
import argparse
import subprocess
import threading
import sys
import yaml
from idf_env import check_idf, get_idf_env
from build import get_build_dir, get_sdkconfig, VALID_TRANSPORTS

def flash_board(chip: str, role: str, port: str, transport: str = "BROADCAST",
                measure: bool = False, project_path: str = "..") -> bool:
    """Flash a pre-built firmware to a board. Returns True on success."""
    build_dir = get_build_dir(chip, role, transport, measure)
    sdkconfig = get_sdkconfig(chip, measure)

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
        print(f"  [FAIL] Flash failed on {port} ({chip}/{role}/{transport}{'+measure' if measure else ''})")
        print(f"         stderr: {result.stderr[-200:]}")
        return False

    print(f"  [OK] Flashed {port} ({chip}/{role}/{transport}{'+measure' if measure else ''})")
    return True

def flash_all(assignments: list, transport: str = "BROADCAST", measure: bool = False,
              project_path: str = "..") -> bool:
    """
    Flash multiple boards in parallel.
    assignments: list of (board_dict, role_str)
    """
    results = {}
    threads = []

    def _flash(board, role):
        ok = flash_board(board["chip"], role, board["port"], transport, measure, project_path)
        results[board["id"]] = ok

    for board, role in assignments:
        t = threading.Thread(target=_flash, args=(board, role))
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

    args = parser.parse_args()
    check_idf()

    if args.port and args.chip and args.role:
        success = flash_board(args.chip, args.role, args.port, args.transport, args.measure, args.project_path)
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
                                  args.transport, args.measure, args.project_path)
            sys.exit(0 if success else 1)
        else:
            print("[ERROR] Please provide --board and --role, or --port, --chip, and --role")
            sys.exit(1)

if __name__ == "__main__":
    main()
