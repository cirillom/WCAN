#!/usr/bin/env python3
"""
Build helper for the WCAN project. Produces firmware variants across:

    chip       in {esp32, esp32c3}
    role       in {SENSOR, RECEIVER, IDLE}
    transport  in {BROADCAST, UNICAST}     (irrelevant for IDLE)
    measure    bool                         (-DMEASURE=1 + sdkconfig.measure overlay)

Build directory layout:
    build_<chip>_idle                                     (transport-agnostic IDLE)
    build_<chip>_<sensor|receiver>_<bcast|unicast>[_measure]

Usage:
    # All chips × roles for one transport
    python build.py --transport BROADCAST
    python build.py --transport UNICAST --measure

    # Single variant
    python build.py esp32 sensor --transport UNICAST
    python build.py esp32c3 receiver --transport BROADCAST --measure
"""
import argparse
import subprocess
import sys
from idf_env import check_idf, get_idf_env

VALID_CHIPS = ("esp32", "esp32c3")
VALID_ROLES = ("SENSOR", "RECEIVER", "IDLE")
VALID_TRANSPORTS = ("BROADCAST", "UNICAST")


def get_build_dir(chip: str, role: str, transport: str = "BROADCAST",
                  measure: bool = False) -> str:
    """Compute the per-variant build directory.

    IDLE binaries don't load WCAN, so transport (and measure) don't change them.
    The directory name reflects this: build_<chip>_idle is shared across runs.
    """
    chip = chip.lower()
    role = role.upper()
    if role == "IDLE":
        return "build" if chip == "esp32" else f"build_{chip}"
    transport_tag = "bcast" if transport.upper() == "BROADCAST" else "unicast"
    suffix = "_measure" if measure else ""
    return f"build_{chip}_{role.lower()}_{transport_tag}{suffix}"


def get_sdkconfig(chip: str, measure: bool = False) -> str:
    """Compute the sdkconfig file name. With measure=True the file is generated
    by ESP-IDF on first build by merging sdkconfig_<chip> with sdkconfig.measure.
    """
    chip = chip.lower()
    if measure:
        return f"sdkconfig.gen_{chip}_measure"
    return f"sdkconfig_{chip}"


def build_variant(chip: str, role: str, transport: str = "BROADCAST",
                  measure: bool = False, project_path: str = "..", sensor_freq: int = 200) -> bool:
    """Build a single firmware variant. Returns True on success."""
    chip = chip.lower()
    role = role.upper()
    transport = transport.upper()

    build_dir = get_build_dir(chip, role, transport, measure)
    sdkconfig = get_sdkconfig(chip, measure)

    print()
    print("=" * 60)
    print(f"  BUILDING: {chip} / {role} / transport={transport} / measure={measure} / sensor_freq={sensor_freq}Hz")
    print(f"  Build dir: {build_dir}")
    print(f"  sdkconfig: {sdkconfig}")
    print("=" * 60)

    cmd = [
        "idf.py",
        "-B", build_dir,
        f"-DROLE={role}",
    ]

    if role == "SENSOR":
        cmd.append(f"-DSENSOR_HZ={sensor_freq}")

    if measure:
        # ESP-IDF SDKCONFIG_DEFAULTS list is semicolon-separated. subprocess
        # passes the whole arg as a single literal — no shell expansion.
        cmd.append(f"-DSDKCONFIG_DEFAULTS=sdkconfig_{chip};sdkconfig.measure")
        cmd.append(f"-DSDKCONFIG={sdkconfig}")
        cmd.append("-DMEASURE=1")
    else:
        cmd.append(f"-DSDKCONFIG={sdkconfig}")

    if role != "IDLE":
        cmd.append(f"-DTRANSPORT={transport}")

    cmd.append("build")

    env = get_idf_env()
    result = subprocess.run(cmd, cwd=project_path, shell=False, env=env)
    if result.returncode != 0:
        print(f"[FAIL] Build failed: {chip}/{role} transport={transport} measure={measure}")
        return False

    print(f"[OK] Build succeeded: {chip}/{role} transport={transport} measure={measure}")
    return True


def build_needed(chips: set, transport: str = "BROADCAST", measure: bool = False,
                 project_path: str = "..", sensor_freq: int = 200) -> bool:
    """Build SENSOR + RECEIVER + IDLE for each chip at the given transport + measure.
    IDLE is built once per chip regardless (transport-agnostic).
    """
    needed = []
    seen_idle = set()
    for chip in chips:
        needed.append((chip, "SENSOR", transport))
        needed.append((chip, "RECEIVER", transport))
        if chip not in seen_idle:
            needed.append((chip, "IDLE", transport))  # transport ignored by IDLE path
            seen_idle.add(chip)

    needed.sort()
    for chip, role, t in needed:
        if not build_variant(chip, role, t, measure, project_path, sensor_freq):
            return False
    return True


def build_all(transport: str = "BROADCAST", measure: bool = False,
              project_path: str = "..") -> bool:
    """Build all roles × all known chips for the given transport + measure combo."""
    return build_needed(set(VALID_CHIPS), transport, measure, project_path)


def main():
    parser = argparse.ArgumentParser(description="WCAN Build Script")
    parser.add_argument("chip", nargs="?", choices=VALID_CHIPS, help="Chip to build for")
    parser.add_argument(
        "role", nargs="?",
        choices=("sensor", "receiver", "idle", "SENSOR", "RECEIVER", "IDLE"),
        help="Role to build",
    )
    parser.add_argument("--transport", default="BROADCAST",
                        choices=VALID_TRANSPORTS,
                        help="Transport variant (default: BROADCAST). Ignored for IDLE.")
    parser.add_argument("--measure", action="store_true",
                        help="Enable measurement instrumentation (-DMEASURE=1 + sdkconfig.measure overlay)")
    parser.add_argument("--project-path", default="..", help="Path to ESP-IDF project root")

    args = parser.parse_args()
    check_idf()

    if args.chip and args.role:
        success = build_variant(args.chip, args.role, args.transport,
                                args.measure, args.project_path)
        sys.exit(0 if success else 1)
    elif not args.chip and not args.role:
        success = build_all(args.transport, args.measure, args.project_path)
        sys.exit(0 if success else 1)
    else:
        print("Please provide both chip and role, or neither to build all.")
        sys.exit(1)


if __name__ == "__main__":
    main()
