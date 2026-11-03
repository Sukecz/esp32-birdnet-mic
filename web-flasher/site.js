'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const boards = ['c3', 's3', 'c5', 'c6', 's31'];
  const kinds = ['web_flash', 'ota', 'manual', 'github_app', 'github_full'];
  const labels = {web_flash:'USB web flasher', ota:'Device OTA', manual:'Direct download', github_app:'GitHub app-only', github_full:'GitHub full image'};
  const number = new Intl.NumberFormat('en');
  let data = null, showAll = false, busy = false;
  const date = value => value ? new Date(value).toLocaleDateString('en', {day:'numeric', month:'short', year:'numeric'}) : '';
  const node = (tag, text, className) => {
    const element = document.createElement(tag);
    if (text != null) element.textContent = text;
    if (className) element.className = className;
    return element;
  };
  function themeLabel() {
    const dark = document.documentElement.dataset.theme === 'dark';
    $('theme-label').textContent = dark ? 'Light' : 'Dark';
    $('theme-icon').textContent = dark ? '☼' : '☾';
    $('theme-toggle').setAttribute('aria-label', 'Switch to ' + (dark ? 'light' : 'dark') + ' mode');
  }
  themeLabel();
  $('theme-toggle').addEventListener('click', () => {
    document.documentElement.dataset.theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    try { localStorage.setItem('birdnetmic-theme', document.documentElement.dataset.theme); } catch (_) {}
    themeLabel();
  });
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', event => {
    try { if (localStorage.getItem('birdnetmic-theme')) return; } catch (_) {}
    document.documentElement.dataset.theme = event.matches ? 'dark' : 'light';
    themeLabel();
  });
  $('ota-form').addEventListener('submit', event => {
    event.preventDefault();
    let host = $('ota-ip').value.trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
    if (!/^[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?(?::\d{1,5})?$/.test(host)) {
      $('ota-status').textContent = 'Enter the device IP address or hostname, e.g. 192.168.1.80.';
      return;
    }
    window.location.href = 'http://' + host + '/ota';
  });
  async function firmware() {
    try {
      const response = await fetch('manifest.json', {cache:'no-store', signal:AbortSignal.timeout(12000)});
      if (!response.ok) throw new Error('manifest');
      const manifest = await response.json();
      $('fw-version-chip').textContent = 'Stable · v' + manifest.version;
    } catch (_) { $('fw-version-chip').textContent = 'Release unavailable'; }
  }
  function selectedKinds() {
    const source = $('source-filter').value;
    if (source === 'all') return kinds;
    if (source === 'website') return ['web_flash', 'ota', 'manual'];
    if (source === 'github') return ['github_app', 'github_full'];
    return [source];
  }
  function value(release, board, kind) { return Number(release.boards[board]?.[kind]) || 0; }
  function sum(release, useBoards, useKinds) {
    return useBoards.reduce((total, board) => total + useKinds.reduce((count, kind) => count + value(release, board, kind), 0), 0);
  }
  function render() {
    if (!data) return;
    const useBoards = $('board-filter').value === 'all' ? boards : [$('board-filter').value];
    const useKinds = selectedKinds();
    const releases = data.versions.filter(release => {
      if (release.version === 'unknown') return true;
      const parts = release.version.split('.').map(Number);
      const cutoff = [1, 9, 1];
      const comparison = cutoff.map((part, index) => (parts[index] || 0) - part).find(delta => delta !== 0) || 0;
      return comparison > 0 || sum(release, boards, kinds) > 0;
    });
    const total = releases.reduce((count, release) => count + sum(release, useBoards, useKinds), 0);
    $('metric-total').textContent = number.format(total);
    const latest = releases.find(release => release.version !== 'unknown');
    $('metric-latest').textContent = number.format(latest ? sum(latest, useBoards, useKinds) : 0);
    $('metric-latest-label').textContent = latest ? 'downloads of v' + latest.version : 'no releases yet';
    $('metric-ota').textContent = number.format(releases.reduce((count, release) => count + sum(release, useBoards, useKinds.filter(kind => kind === 'ota')), 0));
    $('metric-github').textContent = number.format(releases.reduce((count, release) => count + sum(release, useBoards, useKinds.filter(kind => kind.startsWith('github'))), 0));
    $('release-count').textContent = '(' + releases.length + ')';
    const open = new Set([...$('release-list').querySelectorAll('details[open]')].map(element => element.dataset.version));
    const list = document.createDocumentFragment();
    const legend = node('div', null, 'board-legend');
    for (const board of useBoards) {
      const item = node('span');
      item.append(node('i', null, 'board-dot board-' + board), node('span', 'ESP32-' + board.toUpperCase()));
      legend.append(item);
    }
    list.append(legend);
    const max = Math.max(1, ...releases.map(release => sum(release, useBoards, useKinds)));
    for (const release of (showAll ? releases : releases.slice(0, 8))) {
      const details = node('details', null, 'release-row');
      details.dataset.version = release.version;
      details.open = open.has(release.version);
      const summary = node('summary');
      const title = node('span');
      const name = node('span', release.version === 'unknown' ? 'Unassigned' : 'v' + release.version, 'version-name');
      if (release === latest) name.append(node('small', 'Latest'));
      title.append(name, node('span', date(release.published_at) || 'Version unavailable', 'version-date'));
      const bar = node('span', null, 'version-bar');
      bar.setAttribute('aria-hidden', 'true');
      for (const board of useBoards) {
        const segment = node('span', null, 'board-' + board);
        segment.style.width = (100 * sum(release, [board], useKinds) / max) + '%';
        bar.append(segment);
      }
      const count = sum(release, useBoards, useKinds);
      const totalNode = node('span', number.format(count), 'version-total');
      totalNode.setAttribute('aria-label', number.format(count) + ' downloads');
      const chevron = node('span', '›', 'row-chevron');
      chevron.setAttribute('aria-hidden', 'true');
      summary.append(title, bar, totalNode, chevron);
      const breakdown = node('div', null, 'breakdown');
      const scroll = node('div', null, 'table-scroll');
      scroll.tabIndex = 0;
      scroll.setAttribute('role', 'region');
      scroll.setAttribute('aria-label', 'Download breakdown for ' + release.version);
      const table = node('table');
      table.append(node('caption', 'Downloads by type and board'));
      const head = node('thead'), header = node('tr');
      const typeHeading = node('th', 'Download type'); typeHeading.scope = 'col'; header.append(typeHeading);
      for (const label of [...useBoards.map(board => 'ESP32-' + board.toUpperCase()), 'Total']) {
        const th = node('th', label); th.scope = 'col'; header.append(th);
      }
      head.append(header); table.append(head);
      const body = node('tbody');
      for (const kind of useKinds) {
        const row = node('tr');
        const th = node('th', labels[kind]); th.scope = 'row'; row.append(th);
        for (const board of useBoards) row.append(node('td', number.format(value(release, board, kind))));
        row.append(node('td', number.format(sum(release, useBoards, [kind]))));
        body.append(row);
      }
      table.append(body);
      const foot = node('tfoot'), row = node('tr'), th = node('th', 'Total'); th.scope = 'row'; row.append(th);
      for (const board of useBoards) row.append(node('td', number.format(sum(release, [board], useKinds))));
      row.append(node('td', number.format(count))); foot.append(row); table.append(foot);
      scroll.append(table); breakdown.append(scroll); details.append(summary, breakdown); list.append(details);
    }
    $('release-list').replaceChildren(list);
    $('show-older').hidden = releases.length <= 8;
    $('show-older').textContent = showAll ? 'Show recent releases ↑' : 'Show all ' + releases.length + ' releases ↓';
    $('stats-coverage').textContent = 'Website + OTA history starts ' + (date(data.web_since) || 'when the first server records become available') + '. Earlier website downloads are unavailable. GitHub counts cover the full lifetime of each published firmware asset. Website totals refresh every minute; GitHub refreshes hourly.';
  }
  function freshness() {
    const age = Date.now() - new Date(data.updated_at).getTime();
    const ghAge = Date.now() - new Date(data.github_updated_at).getTime();
    $('stats-updated').textContent = 'Updated ' + date(data.updated_at) + ', ' + new Date(data.updated_at).toLocaleTimeString('en', {hour:'2-digit', minute:'2-digit'});
    if (age > 5 * 60000) $('stats-status').textContent = 'Showing saved totals. Website statistics have not refreshed recently.';
    else if (!data.github_available || !data.github_updated_at || ghAge > 2 * 3600000) $('stats-status').textContent = 'Website totals are current. GitHub counts are awaiting a refresh.';
    else $('stats-status').textContent = '';
  }
  async function loadStats() {
    if (busy) return;
    busy = true;
    $('stats-retry').hidden = true;
    try {
      const response = await fetch('download-stats.json', {cache:'no-store', signal:AbortSignal.timeout(12000)});
      if (!response.ok) throw new Error('stats');
      const next = await response.json();
      if (next.schema !== 1 || !Array.isArray(next.versions) || !next.updated_at) throw new Error('format');
      data = next;
      render(); freshness();
    } catch (_) {
      $('stats-status').textContent = data ? 'Showing the last available totals. Could not refresh statistics.' : 'Download statistics are temporarily unavailable. Please try again shortly.';
      $('stats-updated').textContent = data ? 'Last update: ' + date(data.updated_at) : 'Statistics unavailable';
      $('stats-retry').hidden = false;
    } finally { busy = false; }
  }
  $('board-filter').addEventListener('change', render);
  $('source-filter').addEventListener('change', render);
  $('show-older').addEventListener('click', () => { showAll = !showAll; render(); });
  $('stats-retry').addEventListener('click', loadStats);
  firmware(); loadStats();
  setInterval(() => { if (!document.hidden) loadStats(); }, 60000);
})();
