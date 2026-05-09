#!/usr/bin/env python3
import argparse
import subprocess
import sys
from idf_env import check_idf, get_idf_env

BUILD_VARIANTS = {
    ("esp32", "SENSOR"):     ("build_esp32_sensor",     "sdkconfig_esp32"),
    ("esp32", "RECEIVER"):   ("build_esp32_receiver",   "sdkconfig_esp32"),
    ("esp32", "IDLE"):       ("build",                  "sdkconfig_esp32"),
    ("esp32c3", "SENSOR"):   ("build_esp32c3_sensor",   "sdkconfig_esp32c3"),
    ("esp32c3", "RECEIVER"): ("build_esp32c3_receiver", "sdkconfig_esp32c3"),
    ("esp32c3", "IDLE"):     ("build_esp32c3",          "sdkconfig_esp32c3"),
}

def get_build_dir(chip: str, role: str) -> str:
    return BUILD_VARIANTS[(chip.lower(), role.upper())][0]

def get_sdkconfig(chip: str, role: str) -> str:
    return BUILD_VARIANTS[(chip.lower(), role.upper())][1]

def build_variant(chip: str, role: str, project_path: str = "..") -> bool:
    """Build a single firmware variant. Returns True on success."""
    build_dir = get_build_dir(chip, role)
    sdkconfig = get_sdkconfig(chip, role)

    print(f"\n{'='*60}")
    print(f"  BUILDING: {chip} / {role}")
    print(f"  Build dir: {build_dir}")
    print(f"  sdkconfig: {sdkconfig}")
    print(f"{'='*60}")

    cmd = [
        "idf.py",
        "-B", build_dir,
        "--define-cache-entry", f"SDKCONFIG={sdkconfig}",
        f"-DROLE={role.upper()}",
        "build",
    ]

    env = get_idf_env()
    result = subprocess.run(cmd, cwd=project_path, shell=False, env=env)
    if result.returncode != 0:
        print(f"[FAIL] Build failed: {chip}/{role}")
        return False

    print(f"[OK] Build succeeded: {chip}/{role}")
    return True

def build_all(project_path: str = "..") -> bool:
    """Build all variants for all chips."""
    chips = ["esp32", "esp32c3"]
    roles = ["SENSOR", "RECEIVER", "IDLE"]
    
    for chip in chips:
        for role in roles:
            if not build_variant(chip, role, project_path):
                return False
    return True

def build_needed(chips_present: set, project_path: str = "..") -> bool:
    needed = set()
    for chip in chips_present:
        needed.add((chip, "SENSOR"))
        needed.add((chip, "RECEIVER"))
        needed.add((chip, "IDLE"))
        
    for chip, role in sorted(needed):
        if not build_variant(chip, role, project_path):
            return False

    return True

def main():
    parser = argparse.ArgumentParser(description="WCAN Build Script")
    parser.add_argument("chip", nargs="?", choices=["esp32", "esp32c3"], help="Chip to build for")
    parser.add_argument("role", nargs="?", choices=["sensor", "receiver", "idle", "SENSOR", "RECEIVER", "IDLE"], help="Role to build")
    parser.add_argument("--project-path", default="..", help="Path to ESP-IDF project root")
    
    args = parser.parse_args()
    check_idf()

    if args.chip and args.role:
        success = build_variant(args.chip, args.role, args.project_path)
        sys.exit(0 if success else 1)
    elif not args.chip and not args.role:
        success = build_all(args.project_path)
        sys.exit(0 if success else 1)
    else:
        print("Please provide both chip and role, or neither to build all.")
        sys.exit(1)

if __name__ == "__main__":
    main()
