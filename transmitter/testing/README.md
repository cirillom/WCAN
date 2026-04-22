# WCAN Testing

## Overview

The test pipeline is split into two phases: **running** (automated firmware flashing + log capture) and **analysis** (parsing logs, cross-referencing sent vs received CAN counters, generating plots and reports).

---

## Requirements

- ESP-IDF shell with `idf.py` on PATH (`source export.sh` / `export.bat`)
- All Python dependencies managed via `uv` — run `uv sync` once before doing anything else

`pyproject.toml` should declare:

```toml
[project]
dependencies = [
    "pyserial",
    "pyyaml",
    "matplotlib",
]
```

---

## Configuration

Tests are driven by a `boards.yaml` file. Each entry describes one physical board:

```yaml
boards:
  - id: B0
    chip: esp32       # or esp32c3
    port: COM21

  - id: B1
    chip: esp32c3
    port: COM22

test_config:
  duration_seconds: 30
  repeats: 3
  cooldown_seconds: 5
  baud_rate: 115200
  project_path: .
```

At least 2 boards are required. Supported chips are `esp32` and `esp32c3`.

---

## Running Tests

```bash
# Full run: build firmware + execute all tests
uv run test_runner.py

# Skip rebuild (reuse existing firmware binaries)
uv run test_runner.py --skip-build

# Preview the test matrix without executing anything
uv run test_runner.py --dry-run

# Use a custom config file
uv run test_runner.py --config my_boards.yaml
```

### What happens during a run

**1. Build phase** — For each chip type present in the board list, three firmware variants are compiled: `SENSOR`, `RECEIVER`, and `IDLE`. Each variant uses its own build directory and sdkconfig:

| Chip      | Role     | Build dir               | sdkconfig        |
|-----------|----------|-------------------------|------------------|
| esp32     | SENSOR   | `build_esp32_sensor`    | `sdkconfig_esp32`  |
| esp32     | RECEIVER | `build_esp32_receiver`  | `sdkconfig_esp32`  |
| esp32     | IDLE     | `build_esp32_idle`      | `sdkconfig_esp32`  |
| esp32c3   | SENSOR   | `build_esp32c3_sensor`  | `sdkconfig_esp32c3` |
| …         | …        | …                       | …                  |

**2. Test matrix generation** — All valid `(S, R)` combinations are generated where `S ≥ 1`, `R ≥ 1`, and `S + R ≤ N` (N = total boards). Each combination is repeated `repeats` times.

**3. Per-run execution** — For each repetition:
- All boards are flashed in parallel. Active boards (sensors + receivers) get their respective firmware; remaining boards get `IDLE` firmware to keep them silent on the network.
- All active boards are monitored simultaneously via serial at the configured baud rate for `duration_seconds`.
- Logs are written to `results/<timestamp>/test_<S>S-<R>R_rep<N>/`.

**4. Summary** — A `summary.csv` is written at the root of the results directory with one row per test run, recording board assignments, chip types, ports, repeat index, and status (`OK`, `FLASH_FAIL`, or `MONITOR_ERROR`).

### Output structure

```
results/
└── 2026-04-22_174953/
    ├── boards.yaml                  # copy of config used
    ├── summary.csv
    ├── test_1S-1R_rep0/
    │   ├── sensor_B0_COM21.log
    │   └── receiver_B1_COM22.log
    ├── test_1S-1R_rep1/
    │   └── …
    ├── test_2S-1R_rep0/
    │   └── …
    └── …
```

Log filenames follow the pattern `<role>_<board_id>_<port>.log`.

---

## Analyzing Results

```bash
# Analyze an entire timestamped run (all test folders)
uv run analyze_logs.py results/2026-04-22_174953/

# Analyze a single test folder
uv run analyze_logs.py results/2026-04-22_174953/test_2S-3R_rep0/
```

### What the analyzer does

For each test folder it finds:

1. **Parses sensor logs** — extracts the CAN ID and all sent counter values from `ReadDataTask` lines.
2. **Parses receiver logs** — groups received counter values by CAN ID from `USER-RECV` lines.
3. **Cross-references** — for each sensor/receiver pair, compares sent vs received counters for the matching CAN ID. The last sent counter is excluded to avoid false misses caused by timing at monitor shutdown.
4. **Generates a scatter plot** (`analysis.png`) — a grid where rows are sensors and columns are receivers. Each cell shows every sent counter: green dots for received, red crosses for missed.
5. **Writes a text report** (`analysis.txt`) with per-pair stats: sent count, received count, and any missed counter values.

When running over a full timestamped directory, a combined `analysis_summary.txt` is also written at the root level.

### Example text report output

```
=== test_2S-3R_rep0 ===
Sensor B0 (0x1a1) → Receiver B1 (COM22): 149 sent, 149 received, 0 missed
Sensor B0 (0x1a1) → Receiver B2 (COM23): 149 sent, 147 received, 2 missed [74, 103]
Sensor B1 (0x1a2) → Receiver B2 (COM23): 149 sent, 149 received, 0 missed
```

### Output files per test folder

| File            | Description                              |
|-----------------|------------------------------------------|
| `analysis.png`  | Scatter plot grid (received vs missed)   |
| `analysis.txt`  | Human-readable per-pair summary          |

And at the run root (multi-test only):

| File                    | Description                       |
|-------------------------|-----------------------------------|
| `analysis_summary.txt`  | All per-test reports concatenated |