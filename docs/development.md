# Development and validation

The shared target registry is `boards.json`. It describes the four supported XIAO
boards, not every board using the same ESP32 chip. S3 Plus is excluded. Capacities
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
updating release binaries. Both CI and local development compile the six target
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

## Incremental architecture plan

1. **Validation foundation (implemented):** shared board registry, independent
   development builds, memory boundary checks, timed test runner, browser CI and
   versioned packaging. Establish measured timings before optimizing further.
2. **Release provenance (partly implemented):** source/configuration and artifact
   hashes now reject stale `--skip-build` outputs. Add source commit identity. Publish the same
   verified artifacts. Add immutable release staging before replacing public files.
3. **Shared UI state (source extraction implemented):** readable CSS/JS and settings
   fragments now live in `esp32-birdnet-mic/webui/src/`; generation reproduces the
   existing page byte for byte. Direct module tests cover the existing settings
   engine. Continue consolidating feature-specific draft adapters into that engine. Migrate one
   settings group at a time with polling and source/embedded visual checks.
4. **Settings and application state:** extract validated settings and mode transitions
   behind explicit C++ interfaces. Replace source-slicing regressions with tests
   against actual production modules. Keep refactoring separate from behavior changes.
5. **Network/audio boundaries:** keep Web API handlers thin; give capture, processing,
   consumers and lifecycle explicit ownership. Avoid new allocations/virtual layers
   in realtime paths. Retain internal DMA allocation and PSRAM fallback tests.
6. **Measured runtime budgets:** qualify each available board/workload, including
   no-PSRAM fallbacks; automate soak reports and agree release gates from evidence.

Each extraction should be reviewable independently and retain current defaults,
saved settings, API and OTA compatibility. A short module map and tests should
make the next feature require less repository context. Track iteration time,
test time, clean/warm build time and per-board flash growth as the improvement
metrics. Hardware coverage gaps remain explicit rather than inferred from builds.

See [UI source instructions](../esp32-birdnet-mic/webui/README.md) for the module map
and generation commands. Edit the fragments, then regenerate; the runner rejects
stale assembled HTML before running host/browser suites. A source-only extraction
with byte-identical output does not require a new firmware release.
