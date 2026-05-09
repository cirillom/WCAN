import os
import shutil
import sys

def get_idf_env() -> dict:
    """
    Return a clean copy of os.environ for calling idf.py.
    Strips IDF_TARGET (let sdkconfig decide) and any active
    virtualenv so it doesn't conflict with ESP-IDF's own Python.
    """
    env = os.environ.copy()
    env.pop("IDF_TARGET", None)
    env.pop("VIRTUAL_ENV", None)

    # Remove venv bin/Scripts from PATH so idf.py uses system/IDF python
    venv_path = os.environ.get("VIRTUAL_ENV")
    if venv_path and "PATH" in env:
        path_parts = env["PATH"].split(os.pathsep)
        path_parts = [p for p in path_parts if not p.startswith(venv_path)]
        env["PATH"] = os.pathsep.join(path_parts)

    return env

def check_idf():
    """Verify idf.py is available."""
    if shutil.which("idf.py") is None:
        print("[ERROR] idf.py not found on PATH.")
        print("        Run this script from an ESP-IDF terminal (source export.sh / export.bat).")
        sys.exit(1)
    
if __name__ == "__main__":
    check_idf()
    print("[OK] idf.py found")
