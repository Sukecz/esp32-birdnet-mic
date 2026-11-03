# Development and validation

The shared target registry is `boards.json`. It describes four XIAO boards and ESP32-S31-Function-CoreBoard-1, not every
board using the same ESP32 chip. S3 Plus is excluded. Capacities
are bytes; manufacturer KB/MB capacities have been converted as binary memory
capacities. The registry includes manufacturer references. Arduino configuration
and static data limits correspond to the pinned core in `build-dependencies.json`.

Use [the source map](architecture.md) to locate a feature's current owner before editing.

| Target | Physical SRAM | Physical PSRAM | Flash | Current OTA slot |
| --- | ---: | ---: | ---: | ---: |
| C3 | 400 KiB | none | 4 MiB | 1,984 KiB |
| S3 | 512 KiB | 8 MiB, OPI | 8 MiB | 3,264 KiB |
| C5 | 384 KiB | 8 MiB | 8 MiB | 3,264 KiB |
| C6 | 512 KiB | none | 4 MiB | 1,984 KiB |
| S31 CoreBoard-1 | 512 KiB | 16 MiB, fixed OPI | 16 MiB | 3,264 KiB |

This table is explanatory; build tools consume the JSON registry. SRAM includes
instruction and data memory. It is not the amount available to malloc. The static
data limit of 327,680 bytes matches Arduino's target limit, not a safe runtime
allocation budget. The ELF check also compares static IRAM plus data against total
SRAM, but excludes alignment/linker holes. Linker region checks remain necessary.
DMA/internal allocations cannot be budgeted against PSRAM. Existing runtime
reserves and allocation failure handling remain essential.

## Setup

Use Arduino CLI and dependencies pinned in `build-dependencies.json`:

```sh
python3 tools/check_build_dependencies.py --install
python3 -m pip install -r tools/requirements-webui.txt
npm ci --prefix tools --ignore-scripts
tools/node_modules/.bin/playwright-core install chromium
```

Native tests require g++, Node.js 24 and Python 3. Build automation also uses
`size` (binutils) and `flock` (util-linux); the target cache is locked through
compilation and metadata export. Browser tests use the pinned
Playwright Chromium by default; set `CHROMIUM=/path/to/chromium` to reuse an
installed browser. CI installs Linux browser dependencies using `--with-deps`.
Asset/server checks also require nginx. Follow local installation approval rules.

## Commands

```sh
python3 tools/test.py host
python3 tools/test.py browser
python3 tools/test.py browser --filter ui_settings
python3 tools/build.py --board c6
python3 tools/build.py --board s3 --no-psram
python3 tools/build.py --all --matrix
tools/package_firmware.sh
python3 tools/test.py release
```

Host tests include Python regressions, their native C++ harnesses, standalone
native tests and Node tests without a real browser. Browser tests are discovered
from existing `*_test.js` files importing Playwright. Tests must fail on errors;
the runner records per-step status/time in `tmp/test-results/` and returns failure
if any step fails or times out. Browser files currently cover different subsets
of viewport/theme/source/embedded combinations; passing the suite does not mean
every feature has complete visual coverage.

Development builds produce `tmp/build/<target>/` and a memory report without
updating release binaries. Both CI and local development compile the seven target
configurations from the registry. The compile helper retains LTO and the custom
C3/C6 partition table. Cache identity includes the effective FQBN.

Packaging retains existing public filenames, C6 aliases and immutable OTA bridge
behavior. The local `scripts/build_all_firmware.sh` is a compatibility wrapper.
The versioned implementation checks every board's actual ELF, app binary, partition
table, FQBN and build receipt before copying any payload. The local publication
helper also runs `tools/verify_release_inputs.py` and the release contract before
pushing or publishing. `--skip-build` is packaging only. A build receipt binds source/configuration
and exported artifact hashes; stale or modified builds are rejected. This checks
project inputs, not every installed toolchain file; pinned dependency checks remain
required. Always compile release sources before publishing. Release/public checks remain distinct from CI builds.

## Hardware qualification

Compilation proves configuration/flash/static size compatibility. Device tests
must separately cover startup, actual audio, concurrent services, recording,
allocation failure and recovery. Check internal free/minimum/largest block and
PSRAM telemetry during the worst supported workload, not only idle heap. Do not
add unmeasured universal runtime thresholds. Record workload, firmware commit,
board, enabled PSRAM, duration and observed minima before approving such budgets.

Use `tools/check_device_status.py` for the existing read-only capture/time smoke
test. Read the local device inventory before selecting a physical target; C6
streaming validation does not qualify C5 SD recording. Preserve settings and the
C6 external antenna. Publication/OTA authority follows AGENTS.md.

## Implementation status

The planned software foundation is implemented: the board registry, six-target
build/memory gates, shared settings field/save/discard behavior, readable UI
sources, firmware service boundaries, source/commit/artifact receipts and verified
release snapshots. `AppRuntime.h` preserves the existing shared configuration
interface while each service owns its transport, worker or driver state.

Native policy tests include production modules; existing hardware fragment
fixtures remain as compatibility coverage during extraction. This is not a claim
that every legacy test has become an isolated unit test or that all future
architecture work is finished.

Release workflow (commit reviewed sources first so build receipts identify a
clean source revision):

```sh
python3 tools/test.py host
python3 tools/test.py browser
python3 tools/build.py --all --matrix
python3 tools/promote_builds.py
tools/package_firmware.sh --skip-build
python3 tools/test.py release
```

Promotion checks all five standard builds before copying exported files into the
packaging layout, so packaging does not compile a second time. Commit the reviewed
artifacts/validation record before publication. The local publication helper
creates a snapshot using `tools/release_stage.py create`, then both GitHub upload
and the deployment helper consume that same snapshot. Hash verification rejects
changed or stale inputs. The snapshot records the clean source commit and the
publication commit separately; source ZIP builds remain supported but are not
accepted as provenance for this maintainer publication path.

The physical qualification matrix remains open where hardware is unavailable.
C6 is the candidate OTA target. S3 candidate startup/web/memory checks passed without microphone/SD; C3/C5 are unavailable. Use `tools/qualify_runtime.py` and
[hardware qualification](hardware-qualification.md) for repeatable observations;
never replace runtime measurements with total SRAM/PSRAM capacities. Timing JSON
reports and per-board memory reports remain in `tmp/test-results/` and `tmp/build/`.

See [UI source instructions](../esp32-birdnet-mic/webui/README.md) for editing and
generation. Recording, Battery, SD logging and power use the common settings
engine; Wi-Fi/AP-password/antenna flows keep their explicit access/reboot handling.

Before a release, briefly review the main guide, device guide, flasher introduction
and feature help for current behavior and validation limits. Run
`python3 tools/check_release_texts.py` after packaging. The installation sentence
has no hardcoded version; the release badge reads the manifest. Public release
verification checks the served landing-page copy as well as firmware payloads.
Device configuration JavaScript now assembles from `webui/src/js/setup.js` and
shares the draft engine in `settings/fields.js`; regenerate assets before testing.

## ESP32-S31 candidate

See [ESP32-S31-Function-CoreBoard-1 integration](esp32-s31.md) for the local 2.06
candidate, onboard ES8311 microphone, common core migration and qualification
limits. S31 images are not yet published.
