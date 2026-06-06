#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEB_DIR="$ROOT_DIR/web-flasher"
REPO="${GITHUB_REPOSITORY:-Sukecz/esp32-birdnet-mic}"
BRANCH="${BRANCH:-main}"

usage() {
  cat <<EOF
Usage: $0

Pushes $BRANCH, creates/pushes tag v<manifest version>, and creates or updates
the GitHub release for $REPO with web flasher firmware assets.

Environment overrides:
  BRANCH             default: main
  GITHUB_REPOSITORY  default: Sukecz/esp32-birdnet-mic
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$#" -gt 0 ]]; then
  usage >&2
  exit 2
fi

trap 'echo "Release publish failed at line $LINENO: $BASH_COMMAND" >&2' ERR

log() {
  printf '\n== %s ==\n' "$*"
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

version="$(
  python3 - "$WEB_DIR/manifest.json" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["version"])
PY
)"
tag="v$version"

assets=(
  "$WEB_DIR/firmware.bin"
  "$WEB_DIR/firmware-app.bin"
  "$WEB_DIR/manifest.json"
  "$WEB_DIR/ota-version.txt"
  "$WEB_DIR/bootloader.bin"
  "$WEB_DIR/partitions.bin"
  "$WEB_DIR/boot_app0.bin"
)

log "Checking tooling and repository state"
require_command git
require_command gh
require_command python3

cd "$ROOT_DIR"
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Working tree is not clean. Commit the release changes before publishing." >&2
  git status --short
  exit 1
fi

for asset in "${assets[@]}"; do
  if [[ ! -s "$asset" ]]; then
    echo "Missing or empty release asset: $asset" >&2
    exit 1
  fi
done

python3 -m json.tool "$WEB_DIR/manifest.json" >/dev/null
gh auth status >/dev/null

log "Pushing $BRANCH"
git push origin "$BRANCH"
git fetch --tags origin

if git rev-parse "$tag" >/dev/null 2>&1; then
  log "Pushing existing tag $tag"
else
  log "Creating tag $tag"
  git tag -a "$tag" -m "$tag"
fi
git push origin "$tag"

notes_file="$(mktemp)"
trap 'rm -f "$notes_file"' EXIT
python3 - "$ROOT_DIR/esp32-birdnet-mic/CHANGELOG.md" "$version" >"$notes_file" <<'PY'
import pathlib
import re
import sys

changelog = pathlib.Path(sys.argv[1])
version = re.escape(sys.argv[2])
text = changelog.read_text(encoding="utf-8")
match = re.search(rf"^##\s+{version}\b.*?(?=^##\s+|\Z)", text, re.M | re.S)
if match:
    print(match.group(0).strip())
else:
    print(f"{sys.argv[2]} release")
PY

if gh release view "$tag" --repo "$REPO" >/dev/null 2>&1; then
  log "Updating GitHub release $tag"
  gh release edit "$tag" --repo "$REPO" --title "$tag" --notes-file "$notes_file"
  gh release upload "$tag" "${assets[@]}" --repo "$REPO" --clobber
else
  log "Creating GitHub release $tag"
  gh release create "$tag" "${assets[@]}" --repo "$REPO" --title "$tag" --notes-file "$notes_file" --latest
fi

echo "Published $tag to https://github.com/$REPO/releases/tag/$tag"
