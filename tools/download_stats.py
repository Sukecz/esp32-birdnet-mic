#!/usr/bin/env python3
"""Publish anonymous firmware download totals; no events are sent to Umami.

Only completed firmware GETs count. Bootloaders/partition tables are excluded.
Docker timestamps + line hashes make overlapping log reads safe across runs.
The database contains no IP addresses, referrers, or user-agent strings.
"""
import argparse
from collections import defaultdict
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

REPO = "Sukecz/esp32-birdnet-mic"
BOARDS = ("c3", "s3", "c5", "c6", "s31")
KINDS = ("web_flash", "ota", "manual", "github_app", "github_full")
FIRMWARE = re.compile(r"^firmware(?P<app>-app)?(?:-(?P<board>c3|s31|s3|c5|c6))?(?:-(?P<version>\d+(?:\.\d+){1,2}|bridge))?\.bin$")
COMBINED = re.compile(r'^\S+ - \S+ \[([^]]+)\] "(\S+) (\S+) [^"]+" (\d+) (\d+) "([^"]*)" "([^"]*)"')
AUTOMATION = re.compile(r"curl/|wget/|release-verifier|download-stats|python|bot\b|crawler|spider", re.I)


def utcnow():
    return datetime.now(timezone.utc).isoformat()


def version_key(version):
    return tuple(map(int, version.split("."))) if version != "unknown" else (0,)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix="." + path.name)
    try:
        with os.fdopen(fd, "w") as output:
            json.dump(value, output, separators=(",", ":"))
            output.write("\n")
        os.chmod(name, 0o644)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def github_releases(state, now):
    cache = state / "github.json"
    previous = json.loads(cache.read_text()) if cache.exists() else None
    if previous and (now - datetime.fromisoformat(previous["updated_at"])).total_seconds() < 3600:
        return previous
    try:
        releases = []
        for page in range(1, 21):
            request = Request(f"https://api.github.com/repos/{REPO}/releases?per_page=100&page={page}",
                              headers={"User-Agent": "BirdNETMic-download-stats", "Accept": "application/vnd.github+json"})
            with urlopen(request, timeout=25) as response:
                batch = json.load(response)
            for release in batch:
                if release["draft"] or release["prerelease"]:
                    continue
                version = release["tag_name"].lstrip("v.")
                if not re.fullmatch(r"\d+(?:\.\d+){1,2}", version):
                    continue
                releases.append({"version": version, "published_at": release["published_at"],
                                 "assets": [{"name": a["name"], "size": a["size"], "downloads": a["download_count"]}
                                            for a in release["assets"]]})
            if len(batch) < 100:
                break
        result = {"updated_at": now.isoformat(), "releases": releases, "available": True}
        atomic_json(cache, result)
        return result
    except Exception as error:
        print("GitHub refresh unavailable:", type(error).__name__)
        return previous or {"updated_at": None, "releases": [], "available": False}


def parse_line(line):
    """Accept the historical combined format and the new version-aware JSON log."""
    stamp, _, payload = line.strip().partition(" ")
    if not re.match(r"\d{4}-\d\d-\d\dT", stamp):
        return None
    try:
        if payload.startswith('{"firmware_stats":'):
            data = json.loads(payload)
            return dict(timestamp=stamp, time=data["time"], method=data["method"],
                        path=data["uri"], status=int(data["status"]), size=int(data["bytes"]),
                        referer=data["referer"], agent=data["agent"], release=data["release"],
                        served=data["file"])
        match = COMBINED.match(payload)
        if match:
            when, method, path, status, size, referer, agent = match.groups()
            return dict(timestamp=stamp, time=datetime.strptime(when, "%d/%b/%Y:%H:%M:%S %z").isoformat(),
                        method=method, path=path, status=int(status), size=int(size),
                        referer=referer, agent=agent, release=None, served=None)
    except (ValueError, KeyError):
        pass
    return None


def classify(event, releases):
    if event["method"] != "GET" or event["status"] != 200 or AUTOMATION.search(event["agent"]):
        return None
    path = urlsplit(event["path"]).path
    match = FIRMWARE.fullmatch(Path(path).name)
    if not match or path.count("/") > (2 if path.startswith("/updates/") else 1):
        return None
    board = match["board"] or "c6"
    app = bool(match["app"])
    version = match["version"]
    # Use what nginx actually served (legacy recovery URLs can name another version).
    served_match = FIRMWARE.fullmatch(Path(event["served"]).name) if event["served"] else None
    if served_match and served_match["version"]:
        version = served_match["version"]
    candidates = set()
    known_sizes = set()
    for release in releases:
        for asset in release["assets"]:
            asset_match = FIRMWARE.fullmatch(asset["name"])
            if asset_match and bool(asset_match["app"]) == app and (asset_match["board"] or "c6") == board:
                known_sizes.add(asset["size"])
                if asset["size"] == event["size"]:
                    candidates.add(release["version"])
    if known_sizes and event["size"] not in known_sizes:
        return None  # Aborted/short transfers with a misleading HTTP 200.
    if not known_sizes:
        return None  # Wait for release metadata rather than count unverified transfers.
    if version == "bridge":
        version = "1.35"
    if (not app and event["release"]
            and event["release"] not in {release["version"] for release in releases}):
        return None  # Full images share a size; wait for the logged release metadata.
    if version not in candidates:
        if len(candidates) == 1:
            version = next(iter(candidates))
        elif event["release"] in candidates:
            version = event["release"]
        else:
            version = "unknown"  # Never assign historical aliases to today's version.
    if "ESP32HTTPClient" in event["agent"]:
        kind = "ota"
    elif app and urlsplit(event["referer"]).hostname == "esp32mic.msmeteo.cz" and "Mozilla/" in event["agent"]:
        kind = "web_flash"
    else:
        kind = "manual"
    return version, board, kind


def connect(path):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
    db.execute("CREATE TABLE IF NOT EXISTS downloads (id TEXT PRIMARY KEY, time TEXT NOT NULL, version TEXT NOT NULL, board TEXT NOT NULL, kind TEXT NOT NULL)")
    db.execute("CREATE TABLE IF NOT EXISTS pending (id TEXT PRIMARY KEY, event TEXT NOT NULL)")
    return db


def ingest(db, lines, releases):
    # A deployment can precede its GitHub release/cache refresh. Keep anonymous
    # candidates until size metadata arrives rather than lose those transfers.
    for identity, payload in db.execute("SELECT id, event FROM pending").fetchall():
        event = json.loads(payload)
        result = classify(event, releases)
        if result:
            db.execute("INSERT OR IGNORE INTO downloads VALUES (?,?,?,?,?)", (identity, event["time"], *result))
            db.execute("DELETE FROM pending WHERE id=?", (identity,))
    latest = None
    first = None
    for line in lines:
        event = parse_line(line)
        if not event:
            continue
        latest = max(latest or event["timestamp"], event["timestamp"])
        first = min(first or event["time"], event["time"])
        result = classify(event, releases)
        identity = hashlib.sha256(line.rstrip().encode()).hexdigest()
        if result:
            db.execute("INSERT OR IGNORE INTO downloads VALUES (?,?,?,?,?)", (identity, event["time"], *result))
            db.execute("DELETE FROM pending WHERE id=?", (identity,))
        elif (event["method"] == "GET" and event["status"] == 200
              and FIRMWARE.fullmatch(Path(urlsplit(event["path"]).path).name)
              and not AUTOMATION.search(event["agent"])):
            # Store no raw agent, referrer, query string, or client information.
            event["agent"] = "ESP32HTTPClient" if "ESP32HTTPClient" in event["agent"] else ("Mozilla/" if "Mozilla/" in event["agent"] else "")
            event["referer"] = "https://esp32mic.msmeteo.cz/" if urlsplit(event["referer"]).hostname == "esp32mic.msmeteo.cz" else ""
            event["path"] = urlsplit(event["path"]).path
            db.execute("INSERT OR IGNORE INTO pending VALUES (?,?)", (identity, json.dumps(event)))
    if latest:
        db.execute("INSERT OR REPLACE INTO meta VALUES ('cursor',?)", (latest,))
    if first:
        previous = db.execute("SELECT value FROM meta WHERE key='web_since'").fetchone()
        db.execute("INSERT OR REPLACE INTO meta VALUES ('web_since',?)", (min(first, previous[0]) if previous else first,))


def summarize(db, github):
    counts = defaultdict(lambda: defaultdict(lambda: defaultdict(int)))
    published = {}
    for release in github["releases"]:
        version = release["version"]
        published[version] = release["published_at"]
        counts[version]  # Include released versions with zero downloads.
        for asset in release["assets"]:
            match = FIRMWARE.fullmatch(asset["name"])
            if match:
                kind = "github_app" if match["app"] else "github_full"
                counts[version][match["board"] or "c6"][kind] += asset["downloads"]
    for version, board, kind, count in db.execute("SELECT version, board, kind, count(*) FROM downloads GROUP BY version, board, kind"):
        counts[version][board][kind] += count
    versions = []
    for version in sorted(counts, key=version_key, reverse=True):
        boards = {board: {kind: counts[version][board][kind] for kind in KINDS} for board in BOARDS}
        versions.append({"version": version, "published_at": published.get(version),
                         "total": sum(sum(row.values()) for row in boards.values()), "boards": boards})
    since = db.execute("SELECT value FROM meta WHERE key='web_since'").fetchone()
    return {"schema": 1, "updated_at": utcnow(), "github_updated_at": github["updated_at"],
            "github_available": github.get("available", True), "web_since": since[0] if since else None,
            "total": sum(v["total"] for v in versions), "versions": versions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--container", default="esp32mic-web")
    args = parser.parse_args()
    args.state_dir.mkdir(parents=True, exist_ok=True)
    github = github_releases(args.state_dir, datetime.now(timezone.utc))
    # Without metadata, retain all existing statistics and retry on the next run.
    if not github["releases"]:
        raise RuntimeError("Release metadata unavailable; statistics left unchanged")
    with connect(args.state_dir / "downloads.sqlite3") as db:
        cursor = db.execute("SELECT value FROM meta WHERE key='cursor'").fetchone()
        command = ["docker", "logs", "--timestamps"]
        if cursor:
            # Docker emits nanoseconds; datetime uses microseconds. An overlap prevents gaps.
            when = datetime.fromisoformat(re.sub(r"(\.\d{6})\d+", r"\1", cursor[0]).replace("Z", "+00:00"))
            command += ["--since", (when - timedelta(seconds=2)).isoformat()]
        result = subprocess.run(command + [args.container], capture_output=True, text=True, check=True, timeout=45)
        ingest(db, (result.stdout + result.stderr).splitlines(), github["releases"])
        db.commit()  # Commit before publishing; reruns cannot double count.
        payload = summarize(db, github)
    atomic_json(args.output, payload)
    print(f"Published {payload['total']} downloads across {len(payload['versions'])} versions")


if __name__ == "__main__":
    main()
