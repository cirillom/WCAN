#!/usr/bin/env python3
import argparse
import os
import threading
import time
import serial
from datetime import datetime

def _normalize_can_id(can_id) -> int:
    if isinstance(can_id, str):
        return int(can_id.lower().removeprefix("0x"), 16)
    return int(can_id)


def boot_config_line(role: str, options: dict | None = None, transport: str = "BROADCAST") -> str:
    options = options or {}
    role = role.lower()
    parts = ["wcan", "v=1", f"role={role}", f"transport={transport.upper()}"]

    if role == "sensor":
        base = _normalize_can_id(options.get("sensor_base_can_id", 0x100))
        parts.extend([
            f"base=0x{base:x}",
            f"count={int(options.get('sensor_can_id_count', 1))}",
            f"hz={int(options.get('sensor_freq', 200))}",
            f"linger={int(options.get('linger_ms', 100))}",
        ])
    elif role == "receiver":
        if "receiver_filter_ids" not in options or options["receiver_filter_ids"] is None:
            filter_value = "all"
        else:
            ids = [_normalize_can_id(can_id) for can_id in options["receiver_filter_ids"]]
            filter_value = "none" if not ids else ",".join(f"0x{can_id:x}" for can_id in ids)
        parts.append(f"filter={filter_value}")

    return " ".join(parts)


def _timestamp() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def _write_serial_line(f, decoded: str):
    f.write(f"[{_timestamp()}] {decoded}\n")
    f.flush()


def _reset_board(ser):
    ser.dtr = False   # IO0 = HIGH
    ser.rts = True    # EN = LOW
    time.sleep(0.2)
    ser.rts = False   # EN = HIGH
    ser.dtr = False   # IO0 HIGH


def _send_boot_config(ser, f, config_line: str, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    sent = False

    f.write(f"# UART boot config: {config_line}\n")
    f.flush()

    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue
        decoded = line.decode("utf-8", errors="replace").rstrip()
        _write_serial_line(f, decoded)

        if not sent and "WCAN_CFG_WAIT" in decoded:
            ser.write((config_line + "\n").encode("utf-8"))
            ser.flush()
            f.write(f"# UART boot config sent: {config_line}\n")
            f.flush()
            sent = True
            continue

        if "WCAN_CFG_ACK" in decoded:
            return True
        if "WCAN_CFG_NACK" in decoded:
            return False

    f.write("# UART boot config timeout\n")
    f.flush()
    return False


def _assignment_parts(assignment):
    if len(assignment) == 2:
        board, role = assignment
        options = {}
    else:
        board, role, options = assignment
    return board, role, options


def monitor_board(port: str, baud: int, duration: float, log_path: str, stop_event: threading.Event,
                  config_line: str | None = None, ready_event: threading.Event | None = None,
                  start_event: threading.Event | None = None):
    """
    Open serial port, hard-reset the board, capture output to a log file.
    Runs until duration elapses or stop_event is set.
    """
    try:
        ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.1)  # let the port settle after opening

        ser.reset_input_buffer()
        _reset_board(ser)

        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            f.write(f"# Monitor started: {datetime.now().isoformat()}\n")
            f.write(f"# Port: {port}, Baud: {baud}, Duration: {duration}s\n\n")

            if config_line is not None and not _send_boot_config(ser, f, config_line):
                f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")
                if ready_event is not None:
                    ready_event.set()
                ser.close()
                return False

            if ready_event is not None:
                ready_event.set()
            if start_event is not None:
                f.write("# Waiting for coordinated capture start\n")
                f.flush()
                start_event.wait()

            start = time.time()
            while not stop_event.is_set() and (time.time() - start) < duration:
                try:
                    line = ser.readline()
                    if line:
                        decoded = line.decode("utf-8", errors="replace").rstrip()
                        _write_serial_line(f, decoded)
                except serial.SerialException as e:
                    f.write(f"[ERROR] Serial exception: {e}\n")
                    break

            f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")

        ser.close()
        return True

    except serial.SerialException as e:
        with open(log_path, "w") as f:
            f.write(f"# FAILED TO OPEN PORT: {port}\n# Error: {e}\n")
        print(f"  [FAIL] Could not open {port}: {e}")
        if ready_event is not None:
            ready_event.set()
        return False

def monitor_all_boards(boards_with_roles: list, baud: int, duration: float, log_dir: str,
                       transport: str = "BROADCAST") -> bool:
    """
    Start monitoring all boards in parallel threads.
    boards_with_roles: list of (board_dict, role_str) or (board_dict, role_str, options_dict)
    Returns True if all monitors succeeded.
    """
    os.makedirs(log_dir, exist_ok=True)
    stop_event = threading.Event()
    start_event = threading.Event()
    threads = []
    ready_events = {}
    results = {}

    def _start_assignment(assignment):
        board, role, options = _assignment_parts(assignment)
        log_filename = f"{role.lower()}_{board['id']}_{board['port'].replace('/', '_')}.log"
        log_path = os.path.join(log_dir, log_filename)
        config_line = boot_config_line(role, options, transport)
        ready_event = threading.Event()
        ready_events[board["id"]] = ready_event

        def _monitor(p=board["port"], lp=log_path, bid=board["id"], cfg=config_line, ready=ready_event):
            ok = monitor_board(p, baud, duration, lp, stop_event, cfg, ready, start_event)
            results[bid] = ok

        t = threading.Thread(target=_monitor)
        threads.append(t)
        t.start()
        return ready_event

    def _wait_ready(events, timeout=15.0):
        deadline = time.time() + timeout
        for event in events:
            remaining = max(0.0, deadline - time.time())
            if not event.wait(remaining):
                return False
        return True

    def _group_failed(group):
        for assignment in group:
            board, _role, _options = _assignment_parts(assignment)
            if results.get(board["id"]) is False:
                return True
        return False

    normalized = list(boards_with_roles)
    receiver_assignments = [
        assignment for assignment in normalized
        if _assignment_parts(assignment)[1].upper() == "RECEIVER"
    ]
    sensor_assignments = [
        assignment for assignment in normalized
        if _assignment_parts(assignment)[1].upper() == "SENSOR"
    ]
    other_assignments = [
        assignment for assignment in normalized
        if _assignment_parts(assignment)[1].upper() not in ("RECEIVER", "SENSOR")
    ]

    for group in (receiver_assignments, sensor_assignments, other_assignments):
        events = [_start_assignment(assignment) for assignment in group]
        if events and (not _wait_ready(events) or _group_failed(group)):
            stop_event.set()
            start_event.set()
            break

    start_event.set()

    from tqdm import tqdm
    start_t = time.time()
    with tqdm(total=int(duration), desc="  Monitoring", unit="s", leave=False, bar_format="{l_bar}{bar}| {n_fmt}/{total_fmt}s") as pbar:
        while time.time() - start_t < duration:
            time.sleep(0.5)
            elapsed = time.time() - start_t
            pbar.n = min(int(duration), int(elapsed))
            pbar.refresh()
        pbar.n = int(duration)
        pbar.refresh()

    for t in threads:
        t.join(timeout=5)

    stop_event.set()
    return len(results) == len(threads) and all(results.values())

def main():
    parser = argparse.ArgumentParser(description="WCAN Monitor Script")
    parser.add_argument("--port", required=True, help="Serial port to monitor")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate")
    parser.add_argument("--duration", type=float, default=30.0, help="Duration in seconds")
    parser.add_argument("--log-path", required=True, help="Path to save log file")
    parser.add_argument("--boot-config", default=None, help="Optional UART boot config line to send after reset")

    args = parser.parse_args()
    
    stop_event = threading.Event()
    success = monitor_board(args.port, args.baud, args.duration, args.log_path, stop_event, args.boot_config)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    import sys
    main()
