#!/usr/bin/env python3

import os
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
CO_PROCESSOR_ROOT = SCRIPT_DIR.parent
VENV_BIN = CO_PROCESSOR_ROOT / ".venv" / "bin"
WEST = VENV_BIN / "west"
# scan_ble.py needs dbus/gi, which live in the system site-packages, not the
# venv, so it's run with the system interpreter rather than the venv's.
SYSTEM_PYTHON = "/usr/bin/python3"
ZEPHYR_BASE = SCRIPT_DIR / "zephyr"
OPENOCD = "/usr/bin/openocd"
BOARD = "mo/nrf52840"

BLE_APP = "bletester"
BLE_SCAN_SCRIPT = SCRIPT_DIR / BLE_APP / "scan_ble.py"
BLE_SCAN_TIMEOUT = 15

APPS = ["pintester", "bletester", "irtester"]


def run(cmd, env):
    print(f"+ {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, cwd=SCRIPT_DIR, env=env).returncode


def build_and_flash(app_name, env):
    app_dir = SCRIPT_DIR / app_name
    build_dir = app_dir / "build"

    print(f"\n=== Building {app_name} ===")
    rc = run([WEST, "build", "-p", "auto", "-b", BOARD, "-d", build_dir, app_dir], env)
    if rc != 0:
        print(f"Build failed for {app_name} (exit {rc})")
        return rc

    print(f"\n=== Flashing {app_name} ===")
    rc = run([WEST, "flash", "-d", build_dir, "--openocd", OPENOCD], env)
    if rc != 0:
        print(f"Flash failed for {app_name} (exit {rc})")
        return rc

    return 0


def main():
    env = os.environ.copy()
    env["ZEPHYR_BASE"] = str(ZEPHYR_BASE)

    overall_rc = 0

    for i, app_name in enumerate(APPS):
        if i > 0:
            input("\nPress Enter to continue to the next test...")

        listener = None

        if app_name == BLE_APP:
            print("\n=== Starting BLE listener (before flashing) ===")
            listener = subprocess.Popen(
                [SYSTEM_PYTHON, BLE_SCAN_SCRIPT, "--timeout", str(BLE_SCAN_TIMEOUT)]
            )

        rc = build_and_flash(app_name, env)
        if rc != 0:
            overall_rc = rc
            if listener is not None:
                listener.terminate()
            continue

        if listener is not None:
            print("\n=== Waiting for BLE listener to finish ===")
            listener.wait()
            if listener.returncode == 0:
                print("BLE test: PASS")
            else:
                print("BLE test: FAIL")
                overall_rc = overall_rc or listener.returncode

    print("\n=== Done ===")
    sys.exit(overall_rc)


if __name__ == "__main__":
    main()
