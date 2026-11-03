# Release workflow

Use [development and validation](docs/development.md) for the current toolchain,
board registry, test boundaries and build commands. The current firmware version
is defined by `FW_VERSION` in `esp32-birdnet-mic/esp32-birdnet-mic.ino`; published
changes are recorded in the [changelog](esp32-birdnet-mic/CHANGELOG.md).

## Choose checks for the change

- Documentation/tooling changes: run relevant checks. Do not change the firmware
  version, rebuild firmware or install it on devices unless firmware inputs change.
- UI changes: edit `esp32-birdnet-mic/webui/src/`, regenerate assembled/embedded
  assets and validate source and embedded behavior. See the [UI source guide](esp32-birdnet-mic/webui/README.md).
- Firmware changes: use focused checks while editing, then run the final host,
  browser and seven-configuration build gates on the settled source. Follow the
  [development workflow](docs/development.md#implementation-status).

Commit reviewed firmware sources before final builds. Promote verified outputs
with `python3 tools/promote_builds.py`, package with
`tools/package_firmware.sh --skip-build`, then run `python3 tools/test.py release`.
Packaging reuses checked binaries; it does not prove a new build. Review and
commit the resulting artifacts before creating a release snapshot.

Device smoke checks normally take 30-60 seconds. Longer tests need a concrete
reason. Build, hardware validation and publication are separate steps; successful
checks do not grant permission to install or publish firmware.

## Publication

On the maintainer's configured host, the local-only `scripts/publish_release.sh`
creates a verified snapshot, pushes the reviewed commit/tag, publishes all five
board asset sets, deploys the public web/OTA feed and verifies both destinations.
Host-specific scripts and credentials are intentionally absent from source clones.
Follow the applicable authorization rules before running this helper. A failed
publication can leave some destinations updated; inspect state before retrying.

The read-only distribution check is available in every source clone:

```sh
python3 tools/verify_published_release.py <major.minor>
```

It compares GitHub/public artifact hashes and checks the OTA feed, board manifest,
versioned images and compatibility aliases. It does not validate physical audio
or SD recording. Routine documentation/tooling maintenance needs no firmware
release, tag or public OTA deployment.

## Files retained for compatibility

The main UI's assembled HTML and embedded headers are generated, checked inputs
for Arduino builds; retain them alongside their editable sources. Firmware parts,
manual OTA copies and C6 aliases are intentional distribution files.

Older versioned app images preserve existing download URLs and rollback choices.
The immutable v1.35 bridge and its metadata let older devices upgrade across the
C3/C6 partition change. Never delete or replace these files as ordinary cleanup.
See [OTA channels](web-flasher/README.md#updates-after-skipped-releases) and the
[artifact layout](web-flasher/README.md#structure). Historical release notes remain
historical; current implementation guidance belongs in `docs/`.
