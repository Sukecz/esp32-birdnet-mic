// Install test tooling: npm install --prefix /tmp/birdnetmic-ui-check playwright-core
// NODE_PATH=/tmp/birdnetmic-ui-check/node_modules node tests/wifi_ap_ui_test.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {chromium} = require('playwright-core');
(async () => {
  const source = fs.readFileSync('esp32-birdnet-mic/webui/index.html', 'utf8');
  const begin = source.lastIndexOf('<tr>', source.indexOf("id='wifi_ap_mode'"));
  const end = source.indexOf('</tr>', begin) + 5;
  const script = [...source.matchAll(/<script>([\s\S]*?)<\/script>/g)].find(m => m[1].includes('let wifiApState'))[1];
  const state = {ok:true, ssid:'Test <network>', connected:true, bssid:'AA:BB:CC:11:22:33', rssi:-58, locked_bssid:'', recovery:false};
  let scans = 0, saves = [], failScan = false;
  const browser = await chromium.launch({executablePath:process.env.CHROMIUM || '/snap/bin/chromium', headless:true, args:['--no-sandbox']});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.route('http://wifi.test/**', async route => {
      const req = route.request(), url = new URL(req.url());
      let body;
      if (url.pathname === '/') return route.fulfill({contentType:'text/html',body:
        '<table>' + source.slice(begin,end) + '</table><script>' + script +
        "\nfunction mutationFetch(url,body){return fetch(url,{method:'POST',body})}</script>"});
      if (url.pathname === '/api/wifi/ap' && req.method() === 'GET') body = state;
      else if (url.pathname === '/api/wifi/ap') {
        const value = new URLSearchParams(req.postData()).get('bssid');
        saves.push(value); state.locked_bssid = value; body = {ok:true};
      } else if (req.method() === 'POST') { ++scans; body = {ok:!failScan}; }
      else body = {ok:true, scanning:false, aps:[{bssid:state.bssid,rssi:-58},{bssid:'AA:BB:CC:44:55:66',rssi:-75}]};
      return route.fulfill({contentType:'application/json',body:JSON.stringify(body)});
    });
    await page.goto('http://wifi.test/');
    await page.evaluate(() => loadWifiAp());
    assert.equal(await page.locator('#wifi_ap_picker').isVisible(), false);
    assert.equal(await page.locator('#wifi_ap_save').isDisabled(), true);
    assert((await page.locator('#wifi_ap_current').textContent()).includes('Test <network>'));
    await page.selectOption('#wifi_ap_mode', 'locked');
    assert.equal(await page.locator('input[type=radio]:checked').inputValue(), state.bssid);
    await page.click('#wifi_ap_scan');
    await page.waitForFunction(() => !wifiApBusy);
    assert.equal(scans, 1);
    await page.locator('input[value="AA:BB:CC:44:55:66"]').check();
    await page.evaluate(() => loadWifiAp());
    assert.equal(await page.locator('input:checked').inputValue(), 'AA:BB:CC:44:55:66');
    assert.equal(scans, 1); // Status polling must not scan or overwrite the draft.
    await page.click('#wifi_ap_save');
    await page.waitForFunction(() => !wifiApBusy);
    assert.deepEqual(saves, ['AA:BB:CC:44:55:66']);
    assert((await page.locator('#wifi_ap_message').textContent()).includes('Reconnecting'));
    state.bssid = state.locked_bssid;
    await page.evaluate(() => loadWifiAp());
    assert.equal(await page.locator('#wifi_ap_message').textContent(), 'Saved. Connected successfully.');
    await page.selectOption('#wifi_ap_mode', 'auto');
    await page.click('#wifi_ap_save');
    await page.waitForFunction(() => !wifiApBusy);
    assert.deepEqual(saves, ['AA:BB:CC:44:55:66', '']);
    await page.selectOption('#wifi_ap_mode', 'locked');
    failScan = true;
    await page.click('#wifi_ap_scan');
    await page.waitForFunction(() => !wifiApBusy);
    assert((await page.locator('#wifi_ap_message').textContent()).includes('Could not scan'));
    assert.equal(await page.locator('#wifi_ap_scan').isDisabled(), false);
    state.connected = false; state.recovery = true; state.locked_bssid = 'AA:BB:CC:44:55:66';
    await page.evaluate(() => { wifiApDirty = false; return loadWifiAp(); });
    assert((await page.locator('#wifi_ap_current').textContent()).includes('Recovery Wi-Fi active'));
    assert.equal(await page.locator('#wifi_ap_recovery_hint').isVisible(), true);
    assert.deepEqual(errors, []);
    console.log('Browser checks passed: AP selection, scan, draft preservation, save/unlock and recovery UI');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });
