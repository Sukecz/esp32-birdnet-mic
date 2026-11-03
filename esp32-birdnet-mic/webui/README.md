# Web UI source

Edit `src/index.html`, `src/css/` and `src/js/`. The top-level `index.html` is the
assembled, readable compatibility output used by existing browser/contract tests.
Do not edit it directly. The device still serves one embedded page; splitting
sources does not introduce runtime modules, network requests or an application
framework.

From the repository root:

```sh
esp32-birdnet-mic/tools/gen_webui_gzip_header.sh
python3 tools/test.py host
CHROMIUM=/path/to/chromium python3 tools/test.py browser
```

The generator assembles `src/index.html` recursively, then runs the existing
minification and deterministic gzip pipeline. `--check` rejects both stale HTML
and stale embedded headers. `tools/assemble_webui.py` can assemble/check HTML
alone while iterating. Include paths resolve relative to the including file;
missing includes, cycles and paths outside `src/` are errors. CSS cascade order,
script evaluation order and whitespace are retained. Unchanged HTML/headers are
not rewritten, preserving timestamps and avoiding needless firmware recompilation.

| Source | Responsibility |
| --- | --- |
| `src/index.html` | Markup, script/style order and include locations |
| `src/css/` | Shared base styles plus existing audio, layout, setup, recording and overview styles, in their original cascade order |
| `src/js/app.js` | Existing application integration, polling, status rendering and bindings |
| `src/js/settings/fields.js` | Value normalization, confirmed values, edit tracking, polling preservation and dirty rendering |
| `src/js/settings/groups.js` | Group saving, request feedback, concurrent edit protection and discard |
| `src/js/draft-highlights.js` | Recording/Battery/power field mapping into the shared settings engine |
| `src/js/recording.js`, `battery.js`, `antenna.js`, `wifi-ap.js` | Existing feature UI behavior |
| `src/js/browser-audio.js` | Listen playback and recovery |
| Other `src/js/` files | Setup, notices and feature-specific bindings |

These are source fragments sharing the page's existing global scope, not isolated
JavaScript modules. Include markers inside `app.js` preserve the current lexical
scope and initialization order. Direct module tests load the complete production
settings fragments in a VM; browser tests verify the assembled and embedded page.
When changing a fragment, run generation before browser tests. The test runner
fails early if the assembled output is stale.

The initial extraction retained byte-identical output. Version 2.05 then migrates
Recording/Battery/logging/power to the shared saved/draft/save/discard lifecycle.
Behavior changes require regressions, all-board builds and the project's
release/device validation workflow.

Device configuration uses `src/js/setup.js`, assembled into `webui/setup.js`.
It includes the same `settings/fields.js` draft engine as the main page; edit the
source fragment and regenerate both source and embedded outputs. Noise reduction
uses per-setting Apply actions in Audio noise reduction. Each request combines
its row's draft with the other rows' confirmed saved values; unrelated drafts stay
pending. Only the main status poll updates those saved baselines. The graph polls
while its section is open and never synchronizes form settings. High-pass and
noise reduction saves keep each other's drafts pending; microphone recommendations
explicitly review any High-pass changes.
