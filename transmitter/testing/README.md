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

At least 2 boards are required. Supported chips are `esp32` and `esp32c3`. The Python test planner treats `esp32c3` as sensor-only hardware; receiver slots are assigned only to `esp32` boards.

---

## Running Tests

```bash
# Full run: build firmware + execute all tests (default: BROADCAST transport)
uv run test_runner.py

# Skip rebuild (reuse existing firmware binaries)
uv run test_runner.py --skip-build

# Preview the test matrix without executing anything
uv run test_runner.py --dry-run

# Use a custom config file
uv run test_runner.py --config my_boards.yaml

# Pick a transport variant (default BROADCAST; MULTICAST builds the wcan_multicast
# component instead of wcan_broadcast)
uv run test_runner.py --transport MULTICAST --name v_multicast

# Enable measurement instrumentation (-DMEASURE=1 + sdkconfig.measure overlay)
uv run test_runner.py --transport MULTICAST --measure --name v_multicast_m

# Focus only on the multicast path in the multiple-CAN-ID suite
uv run test_runner.py --suite multiple --profile multicast_multiple \
  --name multicast_multiple --analyze

# Run advanced scenario tests: active filtering plus random frequency mixing,
# for both transport variants, with reproducible random assignments.
uv run test_runner.py --scenario advanced --transport BOTH --measure --analyze \
  --seed 20260511 --name advanced_filter_freqmix
```

### Scenario tests

`--scenario active_filtering` flashes receivers with generated CAN-ID allowlists
from `boards.yaml` and writes each run's expected delivery map to
`scenario.json`. The analyzer fails the run if a subscribed sensor is not
received by at least one subscribed receiver, or if a receiver accepts an
unsubscribed CAN ID.

`--scenario frequency_mixing` assigns each sensor a random frequency sampled
without replacement from `test_config.frequency_hz`, so no frequency repeats
inside the same run. Use `--seed` to make those assignments reproducible.

`--scenario advanced` runs active filtering and frequency mixing as separate
scenario suites.

### What happens during a run

**1. Build phase** — For each chip type present in the board list, three firmware variants are compiled: `SENSOR`, `RECEIVER`, and `IDLE`. The transport (`BROADCAST` / `MULTICAST`) is selected via `--transport` and is part of the build directory name. `IDLE` is transport-agnostic.

| Chip      | Transport           | Build dir                                | sdkconfig                       |
|-----------|---------------------|------------------------------------------|---------------------------------|
| esp32     | BROADCAST           | `build_esp32_bcast_runtime`              | `sdkconfig_esp32`               |
| esp32     | MULTICAST           | `build_esp32_multicast_runtime`          | `sdkconfig_esp32`               |
| esp32     | MULTICAST + measure | `build_esp32_multicast_runtime_measure`  | `sdkconfig.gen_esp32_measure`   |
| esp32c3   | BROADCAST           | `build_esp32c3_bcast_runtime`            | `sdkconfig_esp32c3`             |
| esp32c3   | MULTICAST           | `build_esp32c3_multicast_runtime`        | `sdkconfig_esp32c3`             |

`--measure` enables `-DMEASURE=1` in the firmware and overlays `sdkconfig.measure` on top of the chip's base sdkconfig (via `SDKCONFIG_DEFAULTS`). The merged result lives in `sdkconfig.gen_<chip>_measure` and is regenerated on first build.

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
3. **Cross-references (per pair)** — for each sensor/receiver pair, compares sent vs received counters for the matching CAN ID. The last sent counter is excluded to avoid false misses caused by timing at monitor shutdown.
4. **Sensor summary (ANY)** — for each sensor, unions the received sets across all receivers to determine whether each counter was picked up by at least one receiver. This gives a network-level miss count independent of individual receiver failures.
5. **Generates a scatter plot** (`analysis.png`) — a grid where rows are sensors and columns are receivers, plus an extra **ANY** column on the right showing the union view per sensor. Each cell shows every sent counter: green dots for received, red crosses for missed. The ANY column has a bold left border to visually separate it.
6. **Writes a text report** (`analysis.txt`) with per-pair stats followed by a sensor summary section.

When running over a full timestamped directory, a combined `analysis_summary.txt` is also written at the root level.

### Example text report output

```
=== test_2S-3R_rep0 ===
Sensor B0 (0x1a1) → Receiver B1 (COM22): 149 sent, 149 received, 0 missed
Sensor B0 (0x1a1) → Receiver B2 (COM23): 149 sent, 147 received, 2 missed [74, 103]
Sensor B1 (0x1a2) → Receiver B2 (COM23): 149 sent, 149 received, 0 missed

--- Sensor summary ---
Sensor B0 (0x1a1): 149 sent, 149 received by at least one receiver, 0 total misses
Sensor B1 (0x1a2): 149 sent, 149 received by at least one receiver, 0 total misses
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

---

## Thesis comparison workflow

For the thesis comparison study, run both transports back-to-back and let the runner aggregate the results in one shot:

```bash
# Run BROADCAST then MULTICAST, analyze each, and emit the comparison CSV.
uv run test_runner.py --transport BOTH --measure --aggregate --name v_thesis
```

This produces:
- `results/v_thesis_broadcast/` — the BROADCAST variant's full test matrix + `analysis_summary.txt`
- `results/v_thesis_multicast/`  — the MULTICAST variant's full test matrix + `analysis_summary.txt`
- `results/v_thesis_comparison.csv` — one row per `(variant, S, R, repeat)` with all measurement metrics for thesis tables / plots

`--aggregate` implies `--analyze`. `--name` becomes the prefix for both variant folders and the CSV. Without `--name`, the runner uses `<timestamp>_<variant>/` and `<timestamp>_comparison.csv`.

The other flags compose as expected:

```bash
# Same matrix, but skip the rebuild and only run the (1,1) and (1,3) cases
uv run test_runner.py --transport BOTH --measure --aggregate --name v_smoke \
    --skip-build --test 1:1 --test 1:3
```

If you'd rather drive each transport separately (e.g. on different days, different conditions), the lower-level form still works:

```bash
uv run test_runner.py --transport BROADCAST --measure --name v_bcast --analyze
uv run test_runner.py --transport MULTICAST  --measure --name v_multicast --analyze
uv run aggregate_thesis_data.py results/v_bcast results/v_multicast \
    --output results/thesis_comparison.csv
```

The aggregation script emits one CSV row per `(variant, S, R, repeat)` with columns for delivery rate, latency median/p99 (HW-ACK µs for multicast, app-ACK ms for broadcast — different layers, see methodology note below), airtime utilisation, cold-start, and minimum stack HWM. The `variant` column comes from each input directory's basename, so naming runs `v_bcast` / `v_multicast` keeps the table readable.

### Methodology notes (latency)

The two transports measure latency at different layers and the resulting numbers are not directly comparable in absolute terms:

| Variant   | Mechanism                                                           | Units    | Field           |
|-----------|---------------------------------------------------------------------|----------|-----------------|
| BROADCAST | Application-level ACK round-trip (`tick_at_send → ack_recv tick`)   | ms       | `lat_rtt_*`     |
| MULTICAST   | ESP-NOW link-layer HW-ACK turnaround (`esp_timer_get_time → cb`)    | µs       | `lat_cb_*`      |

This is captured intentionally — the comparison shows the latency cost broadcast pays for application-layer reliability, not a measurement artifact. The thesis must disclose the methodology of each variant.
