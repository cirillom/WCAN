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
    parts.extend([
        f"test_duration_ms={int(options.get('test_duration_ms', 30000))}",
        f"host_wait_time_ms={int(options.get('host_wait_time_ms', 5000))}",
    ])

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
    ser.dtr = False
    ser.rts = True
    time.sleep(0.2)
    ser.rts = False
    ser.dtr = False


def _send_boot_config_and_wait_ready(ser, f, config_line: str, timeout: float = 20.0, progress_step=None) -> bool:
    deadline = time.time() + timeout
    sent = False
    acked = False

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
            if progress_step is not None:
                progress_step()
            sent = True
            continue

        if "WCAN_CFG_NACK" in decoded:
            return False
        if "WCAN_CFG_ACK" in decoded:
            acked = True
            continue
        if "WCAN_TEST_ABORT" in decoded:
            return False
        if acked and "WCAN_TEST_READY" in decoded:
            if progress_step is not None:
                progress_step()
            return True

    f.write("# UART boot config/ready timeout\n")
    f.flush()
    return False


def _assignment_parts(assignment):
    if len(assignment) == 2:
        board, role = assignment
        options = {}
    else:
        board, role, options = assignment
    return board, role, options


def monitor_board(port: str, baud: int, log_path: str, stop_event: threading.Event,
                  config_line: str, test_duration_ms: int, host_wait_time_ms: int,
                  ready_event: threading.Event | None = None,
                  start_event: threading.Event | None = None,
                  progress_step=None):
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.1)
        ser.reset_input_buffer()
        _reset_board(ser)

        timeout_s = (test_duration_ms + host_wait_time_ms) / 1000.0
        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            f.write(f"# Monitor started: {datetime.now().isoformat()}\n")
            f.write(f"# Port: {port}, Baud: {baud}, Timeout: {timeout_s:.3f}s after WCAN_TEST_START\n\n")

            if not _send_boot_config_and_wait_ready(ser, f, config_line, progress_step=progress_step):
                f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")
                if ready_event is not None:
                    ready_event.set()
                ser.close()
                return False

            if ready_event is not None:
                ready_event.set()
            if start_event is not None:
                f.write("# Waiting for coordinated test start\n")
                f.flush()
                while not start_event.is_set():
                    if stop_event.is_set():
                        f.write("# Monitor stopped before WCAN_TEST_START because not all devices became ready\n")
                        f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")
                        ser.close()
                        return False
                    time.sleep(0.05)

            if stop_event.is_set():
                f.write("# Monitor stopped before WCAN_TEST_START\n")
                f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")
                ser.close()
                return False

            ser.write(b"WCAN_TEST_START\n")
            ser.flush()
            f.write("# UART test start sent: WCAN_TEST_START\n")
            f.flush()

            start = time.time()
            ok = False
            while not stop_event.is_set() and (time.time() - start) < timeout_s:
                try:
                    line = ser.readline()
                    if not line:
                        continue
                    decoded = line.decode("utf-8", errors="replace").rstrip()
                    _write_serial_line(f, decoded)
                    if "WCAN_TEST_ABORT" in decoded:
                        stop_event.set()
                        break
                    if "WCAN_TEST_END" in decoded:
                        ok = True
                        break
                except serial.SerialException as e:
                    f.write(f"[ERROR] Serial exception: {e}\n")
                    break

            if not ok and not stop_event.is_set():
                f.write("# Monitor timeout waiting for WCAN_TEST_END\n")
            f.write(f"\n# Monitor stopped: {datetime.now().isoformat()}\n")

        ser.close()
        return ok

    except serial.SerialException as e:
        with open(log_path, "w") as f:
            f.write(f"# FAILED TO OPEN PORT: {port}\n# Error: {e}\n")
        print(f"  [FAIL] Could not open {port}: {e}")
        if ready_event is not None:
            ready_event.set()
        return False


def idle_board(port: str, baud: int, config_line: str, timeout: float = 10.0) -> bool:
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.1)
        ser.reset_input_buffer()
        _reset_board(ser)

        with open(os.devnull, "w", encoding="utf-8") as f:
            ok = _send_boot_config_and_wait_ready(ser, f, config_line, timeout=timeout)

        ser.close()
        return ok
    except serial.SerialException as e:
        print(f"  [IDLE] Could not open {port}: {e}")
        return False


def idle_all_boards(boards_with_roles: list, baud: int, transport: str = "BROADCAST",
                    test_duration_ms: int = 30000, host_wait_time_ms: int = 5000,
                    timeout: float = 10.0) -> bool:
    normalized = list(boards_with_roles)
    if not normalized:
        return True

    results = {}
    threads = []
    options = {
        "test_duration_ms": test_duration_ms,
        "host_wait_time_ms": host_wait_time_ms,
    }
    config_line = boot_config_line("IDLE", options, transport)

    print("  [IDLE] Resetting boards into IDLE...")

    def _idle_assignment(assignment):
        board, _, _ = _assignment_parts(assignment)
        results[board["id"]] = idle_board(board["port"], baud, config_line, timeout=timeout)

    for assignment in normalized:
        t = threading.Thread(target=_idle_assignment, args=(assignment,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join(timeout=timeout + 2.0)

    ok = len(results) == len(normalized) and all(results.values())
    if not ok:
        failed = [
            _assignment_parts(assignment)[0]["id"]
            for assignment in normalized
            if not results.get(_assignment_parts(assignment)[0]["id"], False)
        ]
        print(f"  [IDLE] Warning: idle setup failed for {', '.join(failed)}")
    else:
        print(f"  [IDLE] ok {len(results)}/{len(normalized)}")
    return ok


def monitor_all_boards(boards_with_roles: list, baud: int, test_duration_ms: int, host_wait_time_ms: int,
                       log_dir: str, transport: str = "BROADCAST") -> bool:
    os.makedirs(log_dir, exist_ok=True)
    stop_event = threading.Event()
    start_event = threading.Event()
    threads = []
    ready_events = {}
    results = {}
    progress_lock = threading.Lock()
    progress = None

    def _progress_step():
        if progress is None:
            return
        with progress_lock:
            progress.update(1)

    def _start_assignment(assignment):
        board, role, options = _assignment_parts(assignment)
        options = dict(options)
        options["test_duration_ms"] = test_duration_ms
        options["host_wait_time_ms"] = host_wait_time_ms
        log_filename = f"{role.lower()}_{board['id']}_{board['port'].replace('/', '_')}.log"
        log_path = os.path.join(log_dir, log_filename)
        config_line = boot_config_line(role, options, transport)
        ready_event = threading.Event()
        ready_events[board["id"]] = ready_event

        def _monitor(p=board["port"], lp=log_path, bid=board["id"], cfg=config_line, ready=ready_event):
            ok = monitor_board(p, baud, lp, stop_event, cfg, test_duration_ms, host_wait_time_ms, ready, start_event, _progress_step)
            results[bid] = ok

        t = threading.Thread(target=_monitor)
        threads.append(t)
        t.start()
        return ready_event

    def _wait_ready(events, timeout=30.0):
        deadline = time.time() + timeout
        for event in events:
            remaining = max(0.0, deadline - time.time())
            if not event.wait(remaining):
                return False
        return True

    normalized = list(boards_with_roles)

    from tqdm import tqdm
    total_steps = max(1, len(normalized) * 2 + 1)
    with tqdm(total=total_steps, desc="  Test steps", unit="step", leave=False, bar_format="{l_bar}{bar}| {n_fmt}/{total_fmt} {unit}") as pbar:
        progress = pbar
        events = [_start_assignment(assignment) for assignment in normalized]

        all_ready = bool(events) and _wait_ready(events, timeout=30.0)
        any_failed = any(results.get(_assignment_parts(assignment)[0]["id"]) is False for assignment in normalized)

        if all_ready and not any_failed:
            start_event.set()
            while any(t.is_alive() for t in threads) and not stop_event.is_set():
                time.sleep(0.25)
        else:
            stop_event.set()
            start_event.set()

        _progress_step()

        progress = None

    for t in threads:
        t.join(timeout=2)

    stop_event.set()
    return len(results) == len(threads) and all(results.values())


def main():
    parser = argparse.ArgumentParser(description="WCAN Monitor Script")
    parser.add_argument("--port", required=True, help="Serial port to monitor")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate")
    parser.add_argument("--test-duration-ms", type=int, default=30000, help="Device test duration in ms")
    parser.add_argument("--host-wait-time-ms", type=int, default=5000, help="Host grace window in ms")
    parser.add_argument("--log-path", required=True, help="Path to save log file")
    parser.add_argument("--boot-config", required=True, help="UART boot config line to send after reset")
    args = parser.parse_args()

    stop_event = threading.Event()
    success = monitor_board(
        args.port,
        args.baud,
        args.log_path,
        stop_event,
        args.boot_config,
        args.test_duration_ms,
        args.host_wait_time_ms,
    )
    raise SystemExit(0 if success else 1)


if __name__ == "__main__":
    main()
