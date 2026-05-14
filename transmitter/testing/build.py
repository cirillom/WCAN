#!/usr/bin/env python3
"""
Build helper for the WCAN project. Produces runtime-configurable firmware variants across:

    chip       in {esp32, esp32c3}
    transport  in {BROADCAST, MULTICAST}
    measure    bool                         (-DMEASURE=1 + sdkconfig.measure overlay)

Build directory layout:
    build_<chip>_<bcast|multicast>_runtime[_measure]

Usage:
    # All chips for one transport
    python build.py --transport BROADCAST
    python build.py --transport MULTICAST --measure

    # Single variant
    python build.py esp32 --transport MULTICAST
    python build.py esp32c3 --transport BROADCAST --measure
"""
import argparse
import subprocess
import sys
from idf_env import check_idf, get_idf_env

VALID_CHIPS = ("esp32", "esp32c3")
VALID_ROLES = ("RUNTIME",)
VALID_TRANSPORTS = ("BROADCAST", "MULTICAST")


def get_build_dir(chip: str, role: str, transport: str = "BROADCAST",
                  measure: bool = False, sensor_freq: int = 200,
                  receiver_filter_ids=None, sensor_base_can_id: int = 0x100,
                  sensor_can_id_count: int = 1, linger_ms: int = 100) -> str:
    """Compute the runtime firmware build directory."""
    chip = chip.lower()
    transport_tag = "bcast" if transport.upper() == "BROADCAST" else "multicast"
    suffix = "_measure" if measure else ""
    return f"build_{chip}_{transport_tag}_runtime{suffix}"


def get_sdkconfig(chip: str, measure: bool = False) -> str:
    """Compute the sdkconfig file name. With measure=True the file is generated
    by ESP-IDF on first build by merging sdkconfig_<chip> with sdkconfig.measure.
    """
    chip = chip.lower()
    if measure:
        return f"sdkconfig.gen_{chip}_measure"
    return f"sdkconfig_{chip}"


def build_variant(chip: str, role: str, transport: str = "BROADCAST",
                  measure: bool = False, project_path: str = "..", sensor_freq: int = 200,
                  receiver_filter_ids=None, sensor_base_can_id: int = 0x100,
                  sensor_can_id_count: int = 1, linger_ms: int = 100,
                  quiet: bool = False) -> bool:
    """Build a single firmware variant. Returns True on success."""
    chip = chip.lower()
    role = role.upper()
    transport = transport.upper()

    build_dir = get_build_dir(
        chip, role, transport, measure, sensor_freq, receiver_filter_ids,
        sensor_base_can_id, sensor_can_id_count, linger_ms,
    )
    sdkconfig = get_sdkconfig(chip, measure)

    if not quiet:
        print()
        print("=" * 60)
        print(
            f"  BUILDING: {chip} / runtime-configurable / transport={transport} / measure={measure}"
        )
        print(f"  Build dir: {build_dir}")
        print(f"  sdkconfig: {sdkconfig}")
        print("=" * 60)

    cmd = [
        "idf.py",
        "-B", build_dir,
    ]

    if measure:
        # ESP-IDF SDKCONFIG_DEFAULTS list is semicolon-separated. subprocess
        # passes the whole arg as a single literal — no shell expansion.
        cmd.append(f"-DSDKCONFIG_DEFAULTS=sdkconfig_{chip};sdkconfig.measure")
        cmd.append(f"-DSDKCONFIG={sdkconfig}")
        cmd.append("-DMEASURE=1")
    else:
        cmd.append(f"-DSDKCONFIG={sdkconfig}")

    cmd.append(f"-DTRANSPORT={transport}")

    cmd.extend(["reconfigure", "build"])

    env = get_idf_env()
    result = subprocess.run(cmd, cwd=project_path, shell=False, env=env, capture_output=quiet, text=quiet)
    if result.returncode != 0:
        print(f"[FAIL] Build failed: {chip}/runtime transport={transport} measure={measure}")
        if quiet:
            print(f"       build_dir={build_dir} sdkconfig={sdkconfig}")
            if result.stdout:
                print("\n[BUILD STDOUT]")
                print(result.stdout)
            if result.stderr:
                print("\n[BUILD STDERR]")
                print(result.stderr)
        return False

    if not quiet:
        print(f"[OK] Build succeeded: {chip}/runtime transport={transport} measure={measure}")
    return True


def build_needed(chips: set, transport: str = "BROADCAST", measure: bool = False,
                 project_path: str = "..", sensor_freq: int = 200) -> bool:
    """Build one runtime-configurable firmware per chip for the given transport."""
    for chip in sorted(chips):
        if not build_variant(chip, "RUNTIME", transport, measure, project_path, sensor_freq):
            return False
    return True


def build_all(transport: str = "BROADCAST", measure: bool = False,
              project_path: str = "..") -> bool:
    """Build all roles × all known chips for the given transport + measure combo."""
    return build_needed(set(VALID_CHIPS), transport, measure, project_path)


def main():
    parser = argparse.ArgumentParser(description="WCAN Build Script")
    parser.add_argument("chip", nargs="?", choices=VALID_CHIPS, help="Chip to build for")
    parser.add_argument("role", nargs="?", help=argparse.SUPPRESS)
    parser.add_argument("--transport", default="BROADCAST",
                        choices=VALID_TRANSPORTS,
                        help="Transport variant (default: BROADCAST). Ignored for IDLE.")
    parser.add_argument("--measure", action="store_true",
                        help="Enable measurement instrumentation (-DMEASURE=1 + sdkconfig.measure overlay)")
    parser.add_argument("--project-path", default="..", help="Path to ESP-IDF project root")

    args = parser.parse_args()
    check_idf()

    if args.chip:
        success = build_variant(args.chip, "RUNTIME", args.transport,
                                args.measure, args.project_path)
        sys.exit(0 if success else 1)
    elif not args.chip:
        success = build_all(args.transport, args.measure, args.project_path)
        sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
