#!/usr/bin/env python3
import argparse
import subprocess
import threading
import sys
import yaml
from idf_env import check_idf, get_idf_env
from build import get_build_dir, get_sdkconfig, VALID_TRANSPORTS

def flash_board(chip: str, port: str, transport: str = "BROADCAST",
                measure: bool = False, project_path: str = "..") -> bool:
    """Flash a pre-built runtime firmware to a board. Returns True on success."""
    build_dir = get_build_dir(chip, transport, measure)
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
        print(f"  [FAIL] Flash failed on {port} ({chip}/runtime/{transport}{'+measure' if measure else ''})")
        print(f"         stderr: {result.stderr[-200:]}")
        return False

    print(f"  [OK] Flashed {port} ({chip}/runtime/{transport}{'+measure' if measure else ''})")
    return True


def _assignment_board(assignment):
    if isinstance(assignment, dict):
        return assignment
    return assignment[0]


def flash_all(assignments: list, transport: str = "BROADCAST", measure: bool = False,
              project_path: str = "..") -> bool:
    """
    Flash multiple boards in parallel.
    Assignments may be board_dict, (board, role), (board, options), or
    (board, role, options); role/options are runtime UART concerns and are
    intentionally ignored while flashing.
    """
    results = {}
    threads = []

    def _flash(board):
        ok = flash_board(board["chip"], board["port"], transport, measure, project_path)
        results[board["id"]] = ok

    for assignment in assignments:
        board = _assignment_board(assignment)
        t = threading.Thread(target=_flash, args=(board,))
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
    parser.add_argument("--transport", default="BROADCAST", choices=VALID_TRANSPORTS,
                        help="Transport variant of the runtime firmware to flash (default: BROADCAST).")
    parser.add_argument("--measure", action="store_true",
                        help="Flash measurement-instrumented binary (must have been built with --measure).")
    parser.add_argument("--project-path", default="..", help="Path to ESP-IDF project root")

    args = parser.parse_args()
    check_idf()

    if args.port and args.chip:
        success = flash_board(args.chip, args.port, args.transport, args.measure, args.project_path)
        sys.exit(0 if success else 1)

    if args.board or (not args.port and not args.chip):
        with open(args.config, "r") as f:
            cfg = yaml.safe_load(f)
        boards = cfg.get("boards", [])

        if args.board:
            board = next((b for b in boards if b["id"] == args.board), None)
            if not board:
                print(f"[ERROR] Board {args.board} not found in {args.config}")
                sys.exit(1)

            success = flash_board(board["chip"], board["port"], args.transport, args.measure, args.project_path)
            sys.exit(0 if success else 1)
        else:
            print("[ERROR] Please provide --board or --port and --chip")
            sys.exit(1)

if __name__ == "__main__":
    main()
