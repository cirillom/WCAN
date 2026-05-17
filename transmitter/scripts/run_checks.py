#!/usr/bin/env python3
import os
import sys
import subprocess
import glob
import json

def run_cmd(cmd, cwd=None):
    print(f"\nRunning: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        print(f"Command failed with exit code {result.returncode}")
        sys.exit(result.returncode)

def main():
    # 1. Run clang-format
    # Find all cpp and h files
    files_to_format = []
    for d in ['components/wcan', 'main']:
        files_to_format.extend(glob.glob(f"{d}/**/*.cpp", recursive=True))
        files_to_format.extend(glob.glob(f"{d}/**/*.c", recursive=True))
        files_to_format.extend(glob.glob(f"{d}/**/*.h", recursive=True))
        files_to_format.extend(glob.glob(f"{d}/**/*.hpp", recursive=True))

    if files_to_format:
        run_cmd(['clang-format', '-i'] + files_to_format)

    # 2. Run build to get compile_commands.json and check build
    run_cmd(['idf.py', '--define-cache-entry', 'SDKCONFIG=sdkconfig_esp32', '-DTRANSPORT=BROADCAST', 'build'])

    # 3. Clean compile_commands.json for clang-tidy
    cc_path = 'build/compile_commands.json'
    if os.path.exists(cc_path):
        print(f"\nCleaning {cc_path} for clang-tidy...")
        with open(cc_path, 'r') as f:
            data = json.load(f)
        
        flags_to_remove = [
            '-mlongcalls',
            '-fno-shrink-wrap',
            '-fno-tree-switch-conversion',
            '-fstrict-volatile-bitfields'
        ]
        
        for entry in data:
            if 'command' in entry:
                cmd = entry['command']
                for flag in flags_to_remove:
                    cmd = cmd.replace(f" {flag}", "")
                
                # Strip out the target triple if it's explicitly passed
                cmd = cmd.replace(" -target xtensa-esp-elf", "")
                cmd = cmd.replace(" -target xtensa-esp32-elf", "")
                cmd = cmd.replace(" -target xtensa-esp32-unknown-elf", "")
                
                # Replace the ESP GCC compiler path with a generic compiler name
                # This prevents clang-tidy from inferring an unsupported "xtensa" target triple
                parts = cmd.split()
                if "xtensa-esp32-elf-g++" in parts[0] or "xtensa-esp-elf-g++" in parts[0]:
                    parts[0] = "c++"
                elif "xtensa-esp32-elf-gcc" in parts[0] or "xtensa-esp-elf-gcc" in parts[0]:
                    parts[0] = "cc"
                
                # Re-assemble command
                cmd = " ".join(parts)
                
                entry['command'] = cmd
                
        with open(cc_path, 'w') as f:
            json.dump(data, f, indent=2)

    # 4. Run clang-tidy
    wcan_cpp = glob.glob('components/wcan/*.cpp')
    main_cpp = glob.glob('main/*.cpp')
    tidy_files = wcan_cpp + main_cpp
    
    if tidy_files:
        run_cmd(['clang-tidy', '-p', 'build', '--fix'] + tidy_files + ['--header-filter=^.*/components/wcan/.*'])

    # 5. Check if git repository is dirty (for pre-commit use case)
    status = subprocess.run(['git', 'status', '--porcelain'], capture_output=True, text=True)
    if status.stdout.strip():
        print("\n[WARNING] Files were modified. Please stage the changes and try committing again.")
        print(status.stdout)
        sys.exit(1)
        
    print("\nAll checks passed successfully!")

if __name__ == '__main__':
    main()
