#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH_DIR="$ROOT_DIR/esp32-birdnet-mic"
WEB_DIR="$ROOT_DIR/web-flasher"
MANUAL_OTA_DIR="$ROOT_DIR/manual-ota-firmware"
BUILD_ROOT="$SKETCH_DIR/build"

mapfile -t BOARDS < <(python3 "$ROOT_DIR/tools/board_registry.py" --package-list)
[[ "${#BOARDS[@]}" -gt 0 ]] || exit 1

CLEAN=0
SKIP_BUILD=0

usage() {
  cat <<EOF
Usage: $0 [--clean] [--skip-build]

Builds all supported XIAO firmware variants and refreshes:
  web-flasher/bootloader-<board>.bin
  web-flasher/partitions-<board>.bin
  web-flasher/boot_app0-<board>.bin
  web-flasher/firmware-<board>.bin
  web-flasher/firmware-app-<board>.bin
  web-flasher/firmware-app-<board>-<version>.bin
  manual-ota-firmware/firmware-app-<board>.bin

The C6 build is also copied to the legacy unsuffixed aliases.

Options:
  --clean       Remove per-board build directories before compiling.
  --skip-build  Repackage existing build outputs without compiling.
EOF
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      ;;
    --skip-build)
      SKIP_BUILD=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
  shift
done

trap 'echo "Build failed at line $LINENO: $BASH_COMMAND" >&2' ERR

log() {
  printf '\n== %s ==\n' "$*"
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_file() {
  if [[ ! -s "$1" ]]; then
    echo "Missing or empty file: $1" >&2
    exit 1
  fi
}

board_build_dir() {
  local fqbn="$1"
  printf '%s/%s\n' "$BUILD_ROOT" "${fqbn//:/\.}"
}

version="$(
  python3 - "$SKETCH_DIR/esp32-birdnet-mic.ino" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'^\s*#define\s+FW_VERSION\s+"([^"]+)"', text, re.M)
if not match:
    raise SystemExit("FW_VERSION not found")
version = match.group(1)
if not re.fullmatch(r"[0-9]+\.[0-9]+", version):
    raise SystemExit(f"FW_VERSION must use major.minor format, got {version!r}")
print(version)
PY
)"

log "Checking tooling"
require_command arduino-cli
require_command python3

python3 "$ROOT_DIR/tools/check_build_dependencies.py"
"$SKETCH_DIR/tools/gen_webui_gzip_header.sh" --check

mkdir -p "$BUILD_ROOT" "$WEB_DIR" "$MANUAL_OTA_DIR"

for entry in "${BOARDS[@]}"; do
  IFS='|' read -r key chip fqbn <<<"$entry"
  build_dir="$(board_build_dir "$fqbn")"

  if [[ "$CLEAN" -eq 1 ]]; then
    rm -rf "$build_dir"
  fi

  if [[ "$SKIP_BUILD" -eq 0 ]]; then
    log "Compiling $chip ($fqbn)"
    compile_options=()
    if [[ "$CLEAN" -eq 1 ]]; then compile_options+=(--clean); fi
    "$ROOT_DIR/tools/compile_firmware.sh" "${compile_options[@]}" --fqbn "$fqbn" --output-dir "$build_dir"
  else
    log "Packaging existing $chip build"
  fi

  app_bin="$build_dir/esp32-birdnet-mic.ino.bin"
  merged_bin="$build_dir/esp32-birdnet-mic.ino.merged.bin"
  bootloader_bin="$build_dir/esp32-birdnet-mic.ino.bootloader.bin"
  partitions_bin="$build_dir/esp32-birdnet-mic.ino.partitions.bin"
  boot_app0_bin="$build_dir/boot_app0.bin"
  flash_args="$build_dir/flash_args"

  require_file "$app_bin"
  require_file "$merged_bin"
  require_file "$bootloader_bin"
  require_file "$partitions_bin"
  require_file "$boot_app0_bin"
  require_file "$flash_args"

  python3 "$ROOT_DIR/tools/build_receipt.py" verify "$build_dir"
  python3 "$ROOT_DIR/tools/check_memory_budget.py" "$key" "$build_dir"

done

# Validate every target before changing any distributable payload.
for entry in "${BOARDS[@]}"; do
  IFS='|' read -r key chip fqbn <<<"$entry"
  build_dir="$(board_build_dir "$fqbn")"
  app_bin="$build_dir/esp32-birdnet-mic.ino.bin"
  merged_bin="$build_dir/esp32-birdnet-mic.ino.merged.bin"
  bootloader_bin="$build_dir/esp32-birdnet-mic.ino.bootloader.bin"
  partitions_bin="$build_dir/esp32-birdnet-mic.ino.partitions.bin"
  boot_app0_bin="$build_dir/boot_app0.bin"

  cp "$bootloader_bin" "$WEB_DIR/bootloader-$key.bin"
  cp "$partitions_bin" "$WEB_DIR/partitions-$key.bin"
  cp "$boot_app0_bin" "$WEB_DIR/boot_app0-$key.bin"
  cp "$merged_bin" "$WEB_DIR/firmware-$key.bin"
  cp "$app_bin" "$WEB_DIR/firmware-app-$key.bin"
  cp "$app_bin" "$WEB_DIR/firmware-app-$key-$version.bin"
  cp "$app_bin" "$MANUAL_OTA_DIR/firmware-app-$key.bin"

  if [[ "$key" == "c6" ]]; then
    cp "$bootloader_bin" "$WEB_DIR/bootloader.bin"
    cp "$partitions_bin" "$WEB_DIR/partitions.bin"
    cp "$boot_app0_bin" "$WEB_DIR/boot_app0.bin"
    cp "$merged_bin" "$WEB_DIR/firmware.bin"
    cp "$app_bin" "$WEB_DIR/firmware-app.bin"
    cp "$app_bin" "$MANUAL_OTA_DIR/firmware-app.bin"
  fi
done

printf '%s\n' "$version" >"$WEB_DIR/ota-version.txt"

log "Refreshing manifest.json"
python3 - "$WEB_DIR/manifest.json" "$version" "$BUILD_ROOT" "$WEB_DIR" <<'PY'
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
version = sys.argv[2]
build_root = pathlib.Path(sys.argv[3])
web_dir = pathlib.Path(sys.argv[4])

sys.path.insert(0, str(web_dir.parent / "tools"))
from board_registry import boards as registry_boards
boards = [(b["id"], b["chip"], b["fqbn"].replace(":", ".")) for b in registry_boards()]

base = json.loads(manifest_path.read_text(encoding="utf-8"))
base["version"] = version
base["builds"] = []

for key, chip, build_name in boards:
    flash_args = build_root / build_name / "flash_args"
    offsets = {}
    for raw_line in flash_args.read_text(encoding="utf-8").splitlines():
        parts = raw_line.split()
        if len(parts) != 2 or not parts[0].startswith("0x"):
            continue
        offsets[parts[1]] = int(parts[0], 16)

    part_specs = [
        (f"bootloader-{key}.bin", "esp32-birdnet-mic.ino.bootloader.bin"),
        (f"partitions-{key}.bin", "esp32-birdnet-mic.ino.partitions.bin"),
        (f"boot_app0-{key}.bin", "boot_app0.bin"),
        (f"firmware-app-{key}-{version}.bin", "esp32-birdnet-mic.ino.bin"),
    ]
    parts = []
    for public_name, build_name_part in part_specs:
        path = web_dir / public_name
        if not path.exists() or path.stat().st_size == 0:
            raise SystemExit(f"Missing generated web artifact: {path}")
        if build_name_part not in offsets:
            raise SystemExit(f"Missing offset for {build_name_part} in {flash_args}")
        parts.append({"path": public_name, "offset": offsets[build_name_part]})

    base["builds"].append({
        "chipFamily": chip,
        "baud": 115200,
        "parts": parts,
    })

manifest_path.write_text(json.dumps(base, indent=2) + "\n", encoding="utf-8")
PY

python3 "$ROOT_DIR/tools/package_ota_bridge.py"

log "Validating generated artifacts"
python3 -m json.tool "$WEB_DIR/manifest.json" >/dev/null
python3 - "$WEB_DIR" "$MANUAL_OTA_DIR" <<'PY'
import json
import pathlib
import sys

web_dir = pathlib.Path(sys.argv[1])
manual_dir = pathlib.Path(sys.argv[2])
manifest = json.loads((web_dir / "manifest.json").read_text(encoding="utf-8"))
version = manifest["version"]
missing = []
for build in manifest["builds"]:
    for part in build["parts"]:
        path = web_dir / part["path"]
        if not path.exists() or path.stat().st_size == 0:
            missing.append(str(path))
for key in ("c3", "s3", "c5", "c6"):
    stable_app = web_dir / f"firmware-app-{key}.bin"
    versioned_app = web_dir / f"firmware-app-{key}-{version}.bin"
    manual_app = manual_dir / f"firmware-app-{key}.bin"
    for path in (web_dir / f"firmware-{key}.bin", stable_app, versioned_app, manual_app):
        if not path.exists() or path.stat().st_size == 0:
            missing.append(str(path))
    if not missing:
        if stable_app.read_bytes() != versioned_app.read_bytes():
            raise SystemExit(f"Stable and versioned OTA artifacts differ for {key}")
        if stable_app.read_bytes() != manual_app.read_bytes():
            raise SystemExit(f"Web and manual OTA artifacts differ for {key}")
for path in (
    web_dir / "bootloader.bin",
    web_dir / "partitions.bin",
    web_dir / "boot_app0.bin",
    web_dir / "firmware.bin",
    web_dir / "firmware-app.bin",
    manual_dir / "firmware-app.bin",
    web_dir / "ota-version.txt",
):
    if not path.exists() or path.stat().st_size == 0:
        missing.append(str(path))
if missing:
    raise SystemExit("Missing generated artifacts:\n" + "\n".join(missing))
if (web_dir / "ota-version.txt").read_text(encoding="utf-8").strip() != version:
    raise SystemExit("ota-version.txt does not match manifest version")
if (web_dir / "firmware-app-c6.bin").read_bytes() != (web_dir / "firmware-app.bin").read_bytes():
    raise SystemExit("C6 app compatibility alias does not match firmware-app-c6.bin")
if (web_dir / "firmware-c6.bin").read_bytes() != (web_dir / "firmware.bin").read_bytes():
    raise SystemExit("C6 merged compatibility alias does not match firmware-c6.bin")
PY

echo "Built and packaged firmware $version for C3/S3/C5/C6."
