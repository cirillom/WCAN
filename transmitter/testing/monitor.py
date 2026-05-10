#!/usr/bin/env python3
import argparse
import os
import threading
import time
import serial
from datetime import datetime

def monitor_board(port: str, baud: int, duration: float, log_path: str, stop_event: threading.Event):
    """
    Open serial port, hard-reset the board, capture output to a log file.
    Runs until duration elapses or stop_event is set.
    """
    try:
        ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.1)  # let the port settle after opening

        ser.reset_input_buffer()

        ser.dtr = False   # IO0 = HIGH 
        ser.rts = True    # EN = LOW  
        time.sleep(0.2)
        ser.rts = False   # EN = HIGH 
        ser.dtr = False   # IO0 HIGH

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
    return all(results.values())

def main():
    parser = argparse.ArgumentParser(description="WCAN Monitor Script")
    parser.add_argument("--port", required=True, help="Serial port to monitor")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--duration", type=float, default=30.0, help="Duration in seconds")
    parser.add_argument("--log-path", required=True, help="Path to save log file")

    args = parser.parse_args()
    
    stop_event = threading.Event()
    success = monitor_board(args.port, args.baud, args.duration, args.log_path, stop_event)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    import sys
    main()
