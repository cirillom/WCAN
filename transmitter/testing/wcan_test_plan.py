from __future__ import annotations

import random
import sys
from dataclasses import dataclass, field
from typing import Any

from build import VALID_TRANSPORTS
from wcan_test_config import (
    VALID_SUITES,
    board_can_ids,
    format_can_id,
    int_list,
    normalize_can_id,
)


@dataclass(frozen=True)
class BuildSpec:
    chip: str
    role: str
    transport: str
    measure: bool
    sensor_freq: int = 200
    sensor_base_can_id: int = 0x100
    sensor_can_id_count: int = 1
    linger_ms: int = 100
    receiver_filter_ids: tuple[int, ...] | None = None


@dataclass
class RunSpec:
    index: int
    suite: str
    profile: str
    transport: str
    repeat: int
    topology: tuple[int, int]
    frequency_hz: int | None
    sensor_frequencies: dict[str, int]
    linger_ms: int
    can_ids_per_sensor: int
    sensor_boards: list[dict]
    receiver_boards: list[dict]
    idle_boards: list[dict]
    receiver_allowlists: dict[str, list[int]] = field(default_factory=dict)
    expected_receivers: dict[str, list[str]] = field(default_factory=dict)

    @property
    def topology_label(self) -> str:
        return f"{self.topology[0]}S-{self.topology[1]}R"

    @property
    def frequency_label(self) -> str:
        if self.suite == "mixed_frequency":
            return "mixed"
        return f"{self.frequency_hz}Hz"

    @property
    def test_folder_name(self) -> str:
        return f"test_{self.topology_label}_rep{self.repeat}"


@dataclass
class RunSettings:
    duration: int
    repeats: int
    cooldown: int
    baud: int
    project_path: str
    transports: list[str]
    measure: bool
    seed: int | None


@dataclass
class RunPlan:
    boards: list[dict]
    settings: RunSettings
    runs: list[RunSpec]
    build_specs: list[BuildSpec]


def generate_full_matrix(n_boards: int) -> list[tuple[int, int]]:
    cases = []
    for sensors in range(1, n_boards):
        for receivers in range(1, n_boards - sensors + 1):
            cases.append((sensors, receivers))
    return cases


def parse_test_filter(values: list[str] | None, n_boards: int) -> set[tuple[int, int]] | None:
    if not values:
        return None
    selected = set()
    for value in values:
        try:
            sensors, receivers = value.split(":")
            selected.add((int(sensors), int(receivers)))
        except ValueError:
            print(f"[ERROR] Invalid --test value {value!r}. Use S:R, e.g. --test 4:1")
            sys.exit(1)

    valid = set(generate_full_matrix(n_boards))
    invalid = selected - valid
    for sensors, receivers in sorted(invalid):
        print(f"[WARNING] Skipping invalid topology {sensors}S-{receivers}R for {n_boards} board(s).")
    return selected & valid


def resolve_suite_names(tests_cfg: dict, suite_or_group: str) -> list[str]:
    if suite_or_group in VALID_SUITES:
        return [suite_or_group]
    groups = tests_cfg.get("suite_groups", {})
    if suite_or_group in groups:
        suites = list(groups[suite_or_group])
        unknown = [suite for suite in suites if suite not in VALID_SUITES]
        if unknown:
            print(f"[ERROR] Unknown suite(s) in group {suite_or_group}: {', '.join(unknown)}")
            sys.exit(1)
        return suites
    print(f"[ERROR] Unknown suite or suite group: {suite_or_group}")
    sys.exit(1)


def resolve_transports(cli_transport: str | None, defaults: dict, profile: dict) -> list[str]:
    if cli_transport == "BOTH":
        return list(VALID_TRANSPORTS)
    if cli_transport in VALID_TRANSPORTS:
        return [cli_transport]

    transports = profile.get("transports", defaults.get("transports", ["BROADCAST", "UNICAST"]))
    transports = [str(transport).upper() for transport in transports]
    bad = [transport for transport in transports if transport not in VALID_TRANSPORTS]
    if bad:
        print(f"[ERROR] Invalid transport(s) in tests.yaml: {', '.join(bad)}")
        sys.exit(1)
    return transports


def resolve_measure(cli_measure: bool | None, defaults: dict, profile: dict) -> bool:
    if cli_measure is not None:
        return bool(cli_measure)
    return bool(profile.get("measure", defaults.get("measure", False)))


def resolve_topologies(raw_topologies: Any, n_boards: int) -> list[tuple[int, int]]:
    if raw_topologies is None or raw_topologies == "full_matrix":
        return generate_full_matrix(n_boards)

    if not isinstance(raw_topologies, list):
        print("[ERROR] profile topologies must be a list or full_matrix.")
        sys.exit(1)

    cases = []
    for item in raw_topologies:
        sensors = int(item["sensors"])
        receivers = int(item["receivers"])
        if sensors < 1 or receivers < 1 or sensors + receivers > n_boards:
            print(f"[WARNING] Skipping invalid topology {sensors}S-{receivers}R for {n_boards} board(s).")
            continue
        cases.append((sensors, receivers))
    return cases


def merged_suite_config(defaults: dict, suite_cfg: dict, profile_cfg: dict) -> dict:
    merged = dict(defaults)
    merged.update(suite_cfg)
    for key in ("duration_seconds", "repeats", "cooldown_seconds", "baud_rate", "project_path", "topologies"):
        if key in profile_cfg:
            merged[key] = profile_cfg[key]
    if "frequency_hz" in profile_cfg and "frequency_hz" not in suite_cfg:
        merged["frequency_hz"] = profile_cfg["frequency_hz"]
    return merged


def frequency_runs(suite: str, suite_config: dict) -> list[int | None]:
    if suite == "mixed_frequency":
        return [None]
    return int_list(suite_config.get("frequency_hz", [200]), "frequency_hz")


def mixed_frequency_assignment(sensor_boards: list[dict], suite_config: dict, rng: random.Random) -> dict[str, int]:
    pool = int_list(suite_config.get("frequency_hz", []), "mixed_frequency.frequency_hz")
    if len(pool) < len(sensor_boards):
        print(
            f"[ERROR] mixed_frequency needs {len(sensor_boards)} unique frequencies, "
            f"but only {len(pool)} are configured."
        )
        sys.exit(1)
    sampled = rng.sample(pool, len(sensor_boards))
    return {board["id"]: sampled[index] for index, board in enumerate(sensor_boards)}


def active_filter_allowlists(sensor_boards: list[dict], receiver_boards: list[dict]) -> tuple[dict[str, list[int]], dict[str, list[str]]]:
    sensor_ids = [normalize_can_id(board["can_id"]) for board in sensor_boards]
    allowlists: dict[str, list[int]] = {}
    expected: dict[str, list[str]] = {format_can_id(can_id): [] for can_id in sensor_ids}

    for receiver_index, receiver in enumerate(receiver_boards):
        excluded_index = receiver_index % len(sensor_ids)
        allowed = [can_id for index, can_id in enumerate(sensor_ids) if index != excluded_index]
        allowlists[receiver["id"]] = allowed
        for can_id in allowed:
            expected[format_can_id(can_id)].append(receiver["id"])

    return allowlists, expected


def choose_boards(boards: list[dict], sensors: int, receivers: int, rng: random.Random) -> tuple[list[dict], list[dict], list[dict]]:
    shuffled = boards.copy()
    rng.shuffle(shuffled)
    sensor_boards = shuffled[:sensors]
    receiver_boards = shuffled[sensors : sensors + receivers]
    idle_boards = shuffled[sensors + receivers :]
    return sensor_boards, receiver_boards, idle_boards


def build_specs_for_run(run: RunSpec, measure: bool) -> list[BuildSpec]:
    boards = run.sensor_boards + run.receiver_boards + run.idle_boards
    return [
        BuildSpec(chip=board["chip"], role="RUNTIME", transport=run.transport, measure=measure)
        for board in boards
    ]


def build_run_plan(
    boards: list[dict],
    tests_cfg: dict,
    suite_or_group: str,
    profile_name: str,
    cli_transport: str | None,
    test_filter: set[tuple[int, int]] | None,
    measure: bool | None,
    seed: int | None,
) -> RunPlan:
    defaults = tests_cfg["defaults"]
    profiles = tests_cfg["profiles"]
    if profile_name not in profiles:
        print(f"[ERROR] Unknown profile: {profile_name}")
        sys.exit(1)
    profile_cfg = profiles[profile_name]

    transports = resolve_transports(cli_transport, defaults, profile_cfg)
    effective_measure = resolve_measure(measure, defaults, profile_cfg)
    settings = RunSettings(
        duration=int(profile_cfg.get("duration_seconds", defaults.get("duration_seconds", 30))),
        repeats=int(profile_cfg.get("repeats", defaults.get("repeats", 3))),
        cooldown=int(profile_cfg.get("cooldown_seconds", defaults.get("cooldown_seconds", 5))),
        baud=int(profile_cfg.get("baud_rate", defaults.get("baud_rate", 921600))),
        project_path=str(profile_cfg.get("project_path", defaults.get("project_path", "."))),
        transports=transports,
        measure=effective_measure,
        seed=seed,
    )

    suites = resolve_suite_names(tests_cfg, suite_or_group)
    rng = random.Random(seed)
    runs: list[RunSpec] = []
    run_index = 0

    for suite in suites:
        suite_cfg = tests_cfg["suites"][suite]
        suite_config = merged_suite_config(defaults, suite_cfg, profile_cfg)
        topologies = resolve_topologies(suite_config.get("topologies"), len(boards))
        if test_filter is not None:
            topologies = [topology for topology in topologies if topology in test_filter]

        min_sensors = int(suite_config.get("requires_min_sensors", 1))
        topologies = [topology for topology in topologies if topology[0] >= min_sensors]
        if not topologies:
            print(f"[WARNING] No valid topologies for suite={suite} profile={profile_name}.")
            continue

        for transport in transports:
            for frequency in frequency_runs(suite, suite_config):
                for sensors, receivers in topologies:
                    for repeat in range(settings.repeats):
                        sensor_boards, receiver_boards, idle_boards = choose_boards(boards, sensors, receivers, rng)
                        sensor_frequencies = {}
                        receiver_allowlists = {}
                        expected_receivers = {}

                        if suite == "mixed_frequency":
                            sensor_frequencies = mixed_frequency_assignment(sensor_boards, suite_config, rng)
                        if suite == "active_filter":
                            receiver_allowlists, expected_receivers = active_filter_allowlists(
                                sensor_boards, receiver_boards
                            )

                        run_index += 1
                        runs.append(
                            RunSpec(
                                index=run_index,
                                suite=suite,
                                profile=profile_name,
                                transport=transport,
                                repeat=repeat,
                                topology=(sensors, receivers),
                                frequency_hz=frequency,
                                sensor_frequencies=sensor_frequencies,
                                linger_ms=int(suite_config.get("linger_ms", defaults.get("linger_ms", 100))),
                                can_ids_per_sensor=int(suite_config.get("can_ids_per_sensor", 1)),
                                sensor_boards=sensor_boards,
                                receiver_boards=receiver_boards,
                                idle_boards=idle_boards,
                                receiver_allowlists=receiver_allowlists,
                                expected_receivers=expected_receivers,
                            )
                        )

    unique_builds = sorted(
        {spec for run in runs for spec in build_specs_for_run(run, effective_measure)},
        key=lambda spec: (
            spec.chip,
            spec.role,
            spec.transport,
            spec.measure,
        ),
    )
    return RunPlan(boards=boards, settings=settings, runs=runs, build_specs=unique_builds)


def compact_run_dict(run: RunSpec) -> dict:
    return {
        "index": run.index,
        "suite": run.suite,
        "profile": run.profile,
        "transport": run.transport,
        "topology": run.topology_label,
        "repeat": run.repeat,
        "frequency_hz": run.frequency_hz,
        "sensor_frequencies": run.sensor_frequencies,
        "linger_ms": run.linger_ms,
        "can_ids_per_sensor": run.can_ids_per_sensor,
        "sensors": [board["id"] for board in run.sensor_boards],
        "receivers": [board["id"] for board in run.receiver_boards],
        "idle": [board["id"] for board in run.idle_boards],
        "receiver_allowlists": {
            board_id: [format_can_id(can_id) for can_id in ids]
            for board_id, ids in sorted(run.receiver_allowlists.items())
        },
        "expected_receivers": run.expected_receivers,
    }


def compact_build_dict(spec: BuildSpec) -> dict:
    return {
        "chip": spec.chip,
        "role": "RUNTIME",
        "transport": spec.transport,
        "measure": spec.measure,
    }
