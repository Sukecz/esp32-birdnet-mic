#!/usr/bin/env python3
"""Read-only check that a GitHub release and the public OTA feed agree byte for byte.

Uses the published release as the authority, not a potentially rebuilt local binary.
No uploads, installs, restarts or notification messages are performed.
"""
import argparse
import hashlib
import json
import re
import urllib.request


def read(url):
    req = urllib.request.Request(url, headers={"User-Agent": "BirdNETMic-release-verifier", "Cache-Control": "no-cache"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return response.read()


def asset_names(version):
    names = ["manifest.json", "ota-version.txt", "bootloader.bin", "partitions.bin",
             "boot_app0.bin", "firmware.bin", "firmware-app.bin"]
    for board in ("c3", "s3", "c5", "c6"):
        names.extend(f"{part}-{board}.bin" for part in
                     ("bootloader", "partitions", "boot_app0", "firmware", "firmware-app"))
        names.append(f"firmware-app-{board}-{version}.bin")
    return names


def verify(repo, version, base):
    release = json.loads(read(f"https://api.github.com/repos/{repo}/releases/tags/v{version}"))
    if release.get("draft") or release.get("prerelease") or release["tag_name"] != f"v{version}":
        raise ValueError("Expected a published stable release")
    assets = {a["name"]: a for a in release["assets"]}
    required = asset_names(version)
    missing = set(required) - assets.keys()
    if missing:
        raise ValueError("Missing GitHub assets: " + ", ".join(sorted(missing)))
    public = {}
    for name in required:
        data = read(base.rstrip("/") + "/" + name)
        public[name] = data
        expected = assets[name].get("digest")
        if not expected or not expected.startswith("sha256:"):
            expected = "sha256:" + hashlib.sha256(read(assets[name]["browser_download_url"])).hexdigest()
        actual = "sha256:" + hashlib.sha256(data).hexdigest()
        if expected != actual or len(data) != assets[name]["size"]:
            raise ValueError("Public/GitHub mismatch: " + name)
    if public["ota-version.txt"].decode().strip() != version:
        raise ValueError("OTA feed advertises another version")
    manifest = json.loads(public["manifest.json"])
    if manifest["version"] != version:
        raise ValueError("Manifest advertises another version")
    expected_chips = {"ESP32-C3", "ESP32-S3", "ESP32-C5", "ESP32-C6"}
    if {b["chipFamily"] for b in manifest["builds"]} != expected_chips:
        raise ValueError("Manifest must cover all four supported boards")
    for build in manifest["builds"]:
        for part in build["parts"]:
            if part["path"] not in public:
                raise ValueError("Manifest references an unchecked asset")
    for board in ("c3", "s3", "c5", "c6"):
        if public[f"firmware-app-{board}.bin"] != public[f"firmware-app-{board}-{version}.bin"]:
            raise ValueError("Stable/versioned mismatch: " + board)
    for part in ("bootloader", "partitions", "boot_app0", "firmware", "firmware-app"):
        if public[f"{part}.bin"] != public[f"{part}-c6.bin"]:
            raise ValueError("C6 compatibility alias mismatch: " + part)
    print(f"Verified v{version}: {len(required)} GitHub/public assets, OTA feed and four-board manifest agree")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Explicit published major.minor version")
    parser.add_argument("--repo", default="Sukecz/esp32-birdnet-mic")
    parser.add_argument("--base-url", default="http://esp32mic.msmeteo.cz", help="HTTP intentionally checks the actual device OTA path")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+", args.version):
        parser.error("version must be major.minor")
    verify(args.repo, args.version, args.base_url)


if __name__ == "__main__":
    main()
