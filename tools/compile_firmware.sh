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
# Enable each XIAO's actual PSRAM interface; preserve explicit disabled builds.
case "$fqbn" in
  esp32:esp32:XIAO_ESP32C5|esp32:esp32:XIAO_ESP32C5:*|esp32:esp32:XIAO_ESP32S3|esp32:esp32:XIAO_ESP32S3:*)
    if [[ "$fqbn" != *PSRAM=* ]]; then
      psram_option=enabled
      if [[ "$fqbn" == esp32:esp32:XIAO_ESP32S3* ]]; then psram_option=opi; fi
      if [[ "$fqbn" == *:*:*:* ]]; then psram_fqbn="$fqbn,PSRAM=$psram_option"; else psram_fqbn="$fqbn:PSRAM=$psram_option"; fi
      for ((i=0; i<${#args[@]}; i++)); do
        case "${args[i]}" in
          --fqbn|-b) args[i+1]="$psram_fqbn" ;;
          --fqbn=*) args[i]="--fqbn=$psram_fqbn" ;;
        esac
      done
      set -- "${args[@]}"
      fqbn="$psram_fqbn"
    fi
    ;;
esac
properties=()
case "$fqbn" in
  esp32:esp32:XIAO_ESP32C3|esp32:esp32:XIAO_ESP32C3:*|esp32:esp32:XIAO_ESP32C6|esp32:esp32:XIAO_ESP32C6:*)
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
      --build-property upload.maximum_size=2031616
      --build-property "recipe.hooks.prebuild.99.pattern=cp -f \"$ROOT_DIR/tools/partitions/birdnet_4mb.csv\" \"{build.path}/partitions.csv\""
    )
    ;;
esac
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
exec arduino-cli compile "${properties[@]}" "$@" "$ROOT_DIR/esp32-birdnet-mic"
