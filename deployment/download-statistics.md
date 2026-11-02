# Website download statistics

The public web flasher reads `/download-stats.json`. Umami tracks ordinary page
visits only; the obsolete `ota-stats-forwarder.service` must remain disabled.

`tools/download_stats.py` runs on RPI as `birdnetmic-download-stats.service`,
triggered every minute by `birdnetmic-download-stats.timer`. It uses only the
Python standard library, reads `docker logs --timestamps esp32mic-web`, and
refreshes public GitHub release metadata hourly without credentials. The script
is installed at `/home/msrpi/scripts/birdnetmic-download-stats.py`.

Private state lives in `/home/msrpi/.local/share/birdnetmic-stats/`:

- `downloads.sqlite3`: deduplicated transfers, cursor, historical coverage and
  pending transfers awaiting release size metadata. No raw client addresses,
  user agents, referrers or query strings are stored.
- `github.json`: cached public release asset sizes and raw download counters.

The collector atomically publishes aggregate counts to
`/home/msrpi/web-projects/web-flasher/download-stats.json`. The web deploy helper
explicitly preserves this generated file during `rsync --delete`. Do not reset
the database on release: it retains counts beyond Docker log rotation. To move
hosts, preserve this state directory as well as the timer and script.

## Counting

- Each completed HTTP 200 GET of a firmware app or merged full image counts once.
  The response size must match release metadata. HEAD, failures, partial/range
  responses, known crawlers, curl, and release verification traffic are excluded.
- USB web flashes count the app payload only. Bootloaders/partition tables do not
  add downloads. Browser transfers from this site, device OTA, direct downloads,
  GitHub app-only and GitHub merged image downloads remain separate subtypes.
- A release total sums its four boards and all five subtypes. Repeat downloads
  count again. Transfers do not prove installation success or unique devices.
- GitHub asset counters are raw lifetime totals and can include historical
  verification or automation. No unverifiable subtraction is applied.
- Historical aliases are matched by payload size. Ambiguous versions are shown
  as `Unassigned`, never silently attributed to the current release. New JSON
  access logs include the deployed version and served filename. The deploy
  helper fills `__FIRMWARE_VERSION__` from `web-flasher/ota-version.txt`; the
  permanent bridge is always attributed to v1.35, including after later releases.
- Web history begins at the oldest imported retained access record. It is a
  lower bound for earlier releases; GitHub history is independent.

The website displays freshness/coverage and retains its last data on a refresh
error. A GitHub outage retains cached counters; a log read failure leaves the
published JSON and cursor unchanged. Pending metadata is retried on subsequent
runs. Overlapping log reads and process restarts cannot duplicate transfers.

## Validation and operations

```sh
python3 -m unittest discover -s tests -p test_download_stats.py
python3 tools/test.py browser --filter download_stats
ssh rpi5 'systemctl status birdnetmic-download-stats.timer birdnetmic-download-stats.service --no-pager'
ssh rpi5 'journalctl -u birdnetmic-download-stats.service -n 20 --no-pager'
curl -fsS https://esp32mic.msmeteo.cz/download-stats.json
```

Install the two unit files from this directory to `/etc/systemd/system/`, create
the state directory owned by `msrpi` with mode 0700, and copy the Python script
to the path above. Enable the timer with `systemctl enable --now
birdnetmic-download-stats.timer`. Starting a collector manually writes only its
state and public aggregate JSON. Follow project approval rules for deployment.
