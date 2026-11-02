#!/usr/bin/env bash
# Apply LTO only to the two targets with tight default OTA slots.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fqbn=""
args=("$@")
for ((i=0; i<${#args[@]}; i++)); do
  case "${args[i]}" in
    --fqbn|-b) fqbn="${args[i+1]:-}" ;;
    --fqbn=*) fqbn="${args[i]#--fqbn=}" ;;
  esac
done
if [[ -z "$fqbn" ]]; then
  echo "Usage: $0 --fqbn <BOARD_FQBN> [arduino-cli compile options]" >&2
  exit 2
fi
# Registry supplies PSRAM defaults; explicit variants remain selectable.
registry_values="$(python3 - "$ROOT_DIR" "$fqbn" <<'PYREG'
import sys
sys.path.insert(0, sys.argv[1] + '/tools')
from board_registry import boards
base = ':'.join(sys.argv[2].split(':')[:3])
b = next((b for b in boards() if b['fqbn'] == base), None)
if b is None:
    raise SystemExit('Unsupported target: ' + base)
print(b['psram_option'] or '')
print(b['ota_slot_bytes'])
print('1' if b['lto'] else '0')
print(b['partition_csv'] or '')
PYREG
)"
mapfile -t registry <<<"$registry_values"
if [[ -n "${registry[0]}" && "$fqbn" != *PSRAM=* ]]; then
  if [[ "$fqbn" == *:*:*:* ]]; then normalized="$fqbn,PSRAM=${registry[0]}"; else normalized="$fqbn:PSRAM=${registry[0]}"; fi
  for ((i=0; i<${#args[@]}; i++)); do
    case "${args[i]}" in
      --fqbn|-b) args[i+1]="$normalized" ;;
      --fqbn=*) args[i]="--fqbn=$normalized" ;;
    esac
  done
  set -- "${args[@]}"
  fqbn="$normalized"
fi
build_key="${fqbn//[^a-zA-Z0-9._-]/_}"
properties=(--build-path "$ROOT_DIR/esp32-birdnet-mic/build/registry-$build_key")
if [[ "${registry[2]}" == 1 ]]; then
    # SDK linker flags contain -fno-lto; enable LTO at both compilation and linking.
    # A dedicated build path prevents reuse of a precompiled non-LTO Arduino core.
    # Keep these objects separate from the normal S3/C5 core cache.
    build_key="${fqbn//[^a-zA-Z0-9._-]/_}"
    properties=(
      --build-path "$ROOT_DIR/esp32-birdnet-mic/build/lto-$build_key"
      --build-property compiler.c.extra_flags=-flto
      --build-property compiler.cpp.extra_flags=-flto
      --build-property 'compiler.c.elf.extra_flags=-flto -Werror=lto-type-mismatch'
      # Match the app capacity in birdnet_4mb.csv (0x1F0000). The immutable
      # v1.35 bridge remains available for devices with the original layout;
      # the OTA handler rejects a larger app until preparation is complete.
      --build-property "upload.maximum_size=${registry[1]}"
      --build-property "recipe.hooks.prebuild.99.pattern=cp -f \"$ROOT_DIR/${registry[3]}\" \"{build.path}/partitions.csv\""
    )
fi
# Use the same stable-ready SD protocol on every supported board. The optional
# override is only for explicit A/B work; normal builds generate a private copy
# from the pinned core and never patch its installed files.
case "$fqbn" in
  esp32:esp32:XIAO_ESP32C3*|esp32:esp32:XIAO_ESP32S3*|esp32:esp32:XIAO_ESP32C5*|esp32:esp32:XIAO_ESP32C6*)
    sd_library="${BIRDNETMIC_SD_LIBRARY:-}"
    if [[ -z "$sd_library" ]]; then
      sd_library="$(python3 "$ROOT_DIR/tools/prepare_sd_library.py")"
    fi
    [[ -f "$sd_library/library.properties" && -f "$sd_library/src/SD.h" ]] || exit 2
    properties+=(--library "$sd_library")
    ;;
esac
# Serialize a shared target cache, including metadata export. Keep the lock
# outside the cache directory because --clean removes that directory's contents.
mkdir -p "$ROOT_DIR/esp32-birdnet-mic/build/.locks"
exec 9>"$ROOT_DIR/esp32-birdnet-mic/build/.locks/$build_key.lock"
flock 9
source_before="$(python3 "$ROOT_DIR/tools/build_receipt.py" fingerprint)"
arduino-cli compile "${properties[@]}" "$@" "$ROOT_DIR/esp32-birdnet-mic"
# Arduino exports binaries but not the effective build configuration. Include it
# beside exported files so memory validation can reject a mismatched target.
output_dir=""
for ((i=0; i<$#; i++)); do
  case "${args[i]}" in
    --output-dir) output_dir="${args[i+1]:-}" ;;
    --output-dir=*) output_dir="${args[i]#--output-dir=}" ;;
  esac
done
if [[ -n "$output_dir" ]]; then
  for metadata in build.options.json flash_args boot_app0.bin; do
    cp "${properties[1]}/$metadata" "$output_dir/$metadata"
  done
  python3 "$ROOT_DIR/tools/build_receipt.py" record "$output_dir" --expected-source "$source_before"
fi
