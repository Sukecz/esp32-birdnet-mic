// Exercise the actual rendered OTA handler with its generated browser assets.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const {chromium} = require('playwright-core');
const pages = JSON.parse(fs.readFileSync('tmp/ota-rendered-pages.json', 'utf8'));
const header = fs.readFileSync('esp32-birdnet-mic/OtaAssets_gz.h', 'utf8');
function gzip(symbol) {
  const body = header.match(new RegExp(symbol + '\\[\\].*?=\\s*\\{([\\s\\S]*?)\\};'))[1];
  return Buffer.from([...body.matchAll(/0x([0-9a-f]{2})/g)].map(m => parseInt(m[1], 16)));
}
(async () => {
  let state = 'available', requests = [], posts = 0, offline = 0, calls = 0;
  let postMode = 'normal', versionCheck = false, hold = false;
  const server = http.createServer((req, res) => {
    const path = new URL(req.url, 'http://localhost').pathname;
    requests.push({path, method: req.method, csrf: req.headers['x-esp32mic-csrf']});
    if (path === '/ota.js' || path === '/ota.css') {
      res.writeHead(200, {'Content-Type': path.endsWith('js') ? 'application/javascript' : 'text/css', 'Content-Encoding': 'gzip'});
      return res.end(gzip(path.endsWith('js') ? 'OTA_JS_GZ' : 'OTA_CSS_GZ'));
    }
    if (path === '/ota') { res.writeHead(200, {'Content-Type': 'text/html'}); return res.end(pages[versionCheck ? 0 : 2]); }
    if (path === '/api/ota_status') { res.writeHead(200, {'Content-Type': 'application/json'}); return res.end('{"checking":true}'); }
    if (path === '/api/update_space/prepare') {
      if (postMode === 'reject') { res.writeHead(409, {'Content-Type': 'application/json'}); return res.end('{"ok":false,"error":"Nothing was changed. Retry later."}'); }
      if (state === 'working' || state === 'ready') { res.writeHead(200, {'Content-Type': 'application/json'}); return res.end('{"ok":true}'); }
      ++posts;
      state = 'working'; calls = 0;
      if (postMode === 'lost') return req.socket.destroy();
      res.writeHead(200, {'Content-Type': 'application/json'}); return res.end('{"ok":true}');
    }
    if (path === '/api/update_space') {
      if (offline > 0) { --offline; return req.socket.destroy(); }
      if (state === 'working' && ++calls > 2 && !hold) state = 'ready';
      res.writeHead(200, {'Content-Type': 'application/json'});
      return res.end(JSON.stringify({state, capacity: state === 'ready' ? 2031616 : 1310720,
        ota_allowed: state === 'ready' || state === 'unsupported',
        ota_message: state === 'check_failed' ? 'Could not verify update preparation. Please reload this page and try again.' : 'Please prepare your device first using the Prepare device button above.',
        message: state === 'recovery_required' ? 'Keep power connected and retry preparation.' : ''}));
    }
    res.writeHead(404); res.end();
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = 'http://127.0.0.1:' + server.address().port;
  const browser = await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(), headless: true, args: ['--no-sandbox']});
  try {
    for (const scenario of ['normal', 'lost', 'cancel', 'reject', 'reload', 'recovery', 'timeout', 'blocked', 'check_failed', 'ready', 'unsupported']) {
      state = scenario === 'recovery' ? 'recovery_required' : ['ready', 'unsupported', 'check_failed'].includes(scenario) ? scenario : 'available';
      requests = []; posts = 0; offline = 0; postMode = scenario; versionCheck = scenario === 'reload';
      hold = scenario === 'timeout';
      const page = await browser.newPage({viewport: {width: 390, height: 844}}), errors = [];
      page.on('pageerror', e => errors.push(e.message));
      page.on('dialog', dialog => scenario === 'cancel' ? dialog.dismiss() : dialog.accept());
      const initialStatus = page.waitForResponse(r => r.url().endsWith('/api/update_space'));
      await page.goto(origin + '/ota');
      await initialStatus;
      if (scenario === 'unsupported') {
        assert.equal(await page.locator('#update-space').isVisible(), false);
      } else {
        await page.waitForFunction(() => !document.getElementById('update-space').hidden);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
        if (scenario === 'ready') {
          assert.equal(await page.locator('#update-space-prepare').isVisible(), false);
          assert.match(await page.locator('#update-space-title').textContent(), /Ready/);
        } else if (scenario === 'blocked' || scenario === 'check_failed') {
          assert(await page.locator('#ota-upload-form button').isDisabled());
          assert(await page.locator('#ota-install-form button').isDisabled());
          assert(await page.locator('#ota-upload-form input').isDisabled());
          await page.evaluate(() => {
            document.getElementById('ota-install-form').dispatchEvent(new Event('submit', {cancelable:true}));
            document.getElementById('ota-upload-form').dispatchEvent(new Event('submit', {cancelable:true}));
          });
          assert(!requests.some(r => r.path === '/ota/upload' || r.path === '/ota/install'));
          assert.equal(await page.locator('#update-space-prepare').isVisible(), scenario === 'blocked');
          assert.match(await page.locator('#ota-preparation-required').textContent(), scenario === 'blocked' ? /Please prepare your device first/ : /Could not verify/);
        } else {
          if (scenario === 'recovery') assert(await page.locator('#ota-upload-form button').isDisabled());
          if (scenario === 'normal') await page.screenshot({path: 'tmp/update-space-available-mobile.png', fullPage: true});
          await page.locator('#update-space-prepare').click();
          if (scenario === 'cancel') {
            assert.equal(posts, 0); assert.equal(await page.locator('#ota-progress').isVisible(), false);
          } else if (scenario === 'reject') {
            await page.waitForFunction(() => document.getElementById('update-space-result').textContent.includes('Nothing was changed'));
            assert.equal(await page.locator('#ota-progress').isVisible(), false);
          } else {
            await page.waitForFunction(() => document.getElementById('ota-progress').classList.contains('active'));
            if (scenario === 'timeout') {
              // Expire the deadline without waiting three real minutes.
              await page.waitForTimeout(100);
              await page.evaluate(() => { const now = Date.now; Date.now = () => now() + 180001; });
              await page.locator('#ota-reconnect').waitFor({state: 'visible'});
              assert.equal(await page.locator('#ota-reconnect').isDisabled(), false);
              hold = false; calls = 2;
              await page.locator('#ota-reconnect').click();
            }
            if (scenario === 'reload') {
              offline = 1;
              await page.reload();
              await page.waitForFunction(() => document.getElementById('ota-progress').classList.contains('active') || document.getElementById('update-space-result').textContent.includes('Preparation complete'));
            }
            await page.waitForFunction(() => document.getElementById('update-space-result').textContent.includes('Preparation complete'), {timeout: 20000});
            assert.equal(posts, 1);
            assert.equal(await page.locator('#ota-progress').isVisible(), false);
            assert.equal(await page.locator('#ota-upload-form button').isDisabled(), false);
            assert.equal(await page.evaluate(() => sessionStorage.getItem('updateSpacePending')), null);
            assert.equal(requests.find(r => r.path.endsWith('/prepare')).csrf, '1');
            if (scenario === 'normal') {
              await page.screenshot({path: 'tmp/update-space-complete-mobile.png', fullPage: true});
            }
          }
        }
      }
      assert.deepEqual(errors, [], scenario);
      await page.close();
      console.log('Update preparation browser:', scenario, 'passed');
    }
  } finally { await browser.close(); server.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
