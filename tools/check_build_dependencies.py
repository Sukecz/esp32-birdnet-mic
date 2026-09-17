#!/usr/bin/env python3
"""Check installed build dependencies; --install is intended for fresh CI runners."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def cli(*args):
    return json.loads(subprocess.check_output(["arduino-cli", *args, "--format", "json"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    lock = json.loads((ROOT / "build-dependencies.json").read_text())
    version = cli("version")["VersionString"]
    if version != lock["arduino_cli"]:
        raise SystemExit(f"Arduino CLI: expected {lock['arduino_cli']}, found {version}")
    if args.install:
        subprocess.run(["arduino-cli", "core", "update-index"], check=True)
        subprocess.run(["arduino-cli", "core", "install", lock["core"]["id"] + "@" + lock["core"]["version"]], check=True)
        for name, version in lock["libraries"].items():
            subprocess.run(["arduino-cli", "lib", "install", name + "@" + version], check=True)
    cores = {p["id"]: p.get("installed_version") for p in cli("core", "list")["platforms"]}
    libraries = {p["library"]["name"]: p["library"]["version"] for p in cli("lib", "list")["installed_libraries"]}
    expected = {lock["core"]["id"]: lock["core"]["version"], **lock["libraries"]}
    installed = {**cores, **libraries}
    for name, version in expected.items():
        if installed.get(name) != version:
            raise SystemExit(f"{name}: expected {version}, found {installed.get(name, 'missing')}")
    print("Build dependencies match build-dependencies.json")


if __name__ == "__main__":
    main()
