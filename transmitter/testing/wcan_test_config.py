from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import yaml

VALID_CHIPS = ("esp32", "esp32c3")
RECEIVER_CAPABLE_CHIPS = ("esp32",)
VALID_SUITES = ("baseline", "multiple", "real_time", "active_filter", "mixed_frequency")


def normalize_can_id(can_id: Any) -> int:
    if isinstance(can_id, str):
        return int(can_id.lower().removeprefix("0x"), 16)
    return int(can_id)


def format_can_id(can_id: Any) -> str:
    return f"0x{normalize_can_id(can_id):x}"


def is_receiver_capable(board: dict) -> bool:
    return board.get("chip") in RECEIVER_CAPABLE_CHIPS


def board_can_ids(board: dict, count: int) -> list[int]:
    base = normalize_can_id(board["can_id"])
    return [base + index for index in range(count)]


def load_yaml(path: str | Path) -> dict:
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    except FileNotFoundError:
        print(f"[ERROR] Config file not found: {path}")
        sys.exit(1)
    if not isinstance(data, dict):
        print(f"[ERROR] Config file must contain a YAML mapping: {path}")
        sys.exit(1)
    return data


def load_boards(path: str | Path) -> list[dict]:
    cfg = load_yaml(path)
    boards = cfg.get("boards", [])
    if not isinstance(boards, list) or len(boards) < 2:
        print(f"[ERROR] Need at least 2 boards in {path}")
        sys.exit(1)

    seen_ids = set()
    for board in boards:
        board_id = str(board.get("id", ""))
        if len(board_id) != 1 or not board_id.isalpha() or board_id.upper() != board_id:
            print(f"[ERROR] Board id must be one uppercase letter, got: {board_id!r}")
            sys.exit(1)
        if board_id in seen_ids:
            print(f"[ERROR] Duplicate board id: {board_id}")
            sys.exit(1)
        seen_ids.add(board_id)

        if board.get("chip") not in VALID_CHIPS:
            print(f"[ERROR] Board {board_id} has invalid chip {board.get('chip')!r}.")
            sys.exit(1)
        if "port" not in board:
            print(f"[ERROR] Board {board_id} is missing port.")
            sys.exit(1)
        if "can_id" not in board:
            print(f"[ERROR] Board {board_id} is missing can_id.")
            sys.exit(1)

        base_id = normalize_can_id(board["can_id"])
        if board_id not in "ABCDEF":
            print(f"[ERROR] Board {board_id} cannot be represented as a hex CAN-ID prefix.")
            sys.exit(1)
        expected_prefix = int(board_id.lower(), 16) << 4
        if (base_id & 0xF0) != expected_prefix:
            print(
                f"[ERROR] Board {board_id} can_id should start with 0x{board_id.lower()} "
                f"(example 0x{board_id.lower()}0); got {format_can_id(base_id)}."
            )
            sys.exit(1)

        board["id"] = board_id
        board["can_id"] = base_id

    return boards


def load_tests(path: str | Path) -> dict:
    cfg = load_yaml(path)
    for key in ("defaults", "profiles", "suites"):
        if key not in cfg or not isinstance(cfg[key], dict):
            print(f"[ERROR] {path} is missing required mapping: {key}")
            sys.exit(1)

    for suite in VALID_SUITES:
        if suite not in cfg["suites"]:
            print(f"[ERROR] {path} is missing suite: {suite}")
            sys.exit(1)

    return cfg


def int_list(values: Any, field_name: str) -> list[int]:
    if values is None:
        return []
    if not isinstance(values, list):
        print(f"[ERROR] {field_name} must be a list.")
        sys.exit(1)
    return [int(value) for value in values]


def scalar_or_list(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    return [value]
