
let wifiApState = null, wifiApDraft = '', wifiApDirty = false, wifiApBusy = false;
let wifiApResults = [], wifiApMessage = '', wifiApExpected = null;
const apNode = id => document.getElementById(id);
function wifiApModeChanged() {
  if (apNode('wifi_ap_mode').value === 'locked' && !wifiApDraft && wifiApState)
    wifiApDraft = wifiApState.locked_bssid || wifiApState.bssid || '';
  updateWifiApDirty();renderWifiAp();
}
function updateWifiApDirty(){
 const value=apNode('wifi_ap_mode').value==='locked'?wifiApDraft:'';
 wifiApDirty=!!wifiApState&&value!==(wifiApState.locked_bssid||'');
}
function renderWifiAp() {
  const locked = apNode('wifi_ap_mode').value === 'locked';
  apNode('wifi_ap_picker').hidden = !locked;
  apNode('wifi_ap_recovery_hint').hidden = false;
  apNode('wifi_ap_mode').disabled = wifiApBusy;
  apNode('wifi_ap_scan').disabled = wifiApBusy;
  apNode('wifi_ap_save').disabled = wifiApBusy || !wifiApState || !wifiApDirty || (locked && !wifiApDraft);
  const list = apNode('wifi_ap_results');
  list.replaceChildren();
  const rows = wifiApResults.slice();
  if (wifiApDraft && !rows.some(ap => ap.bssid === wifiApDraft))
    rows.push({bssid: wifiApDraft, rssi: null});
  for (const ap of rows) {
    const label = document.createElement('label');
    label.style.cssText = 'display:block;margin:8px 0;cursor:pointer';
    const radio = document.createElement('input');
    radio.type = 'radio'; radio.name = 'wifi_ap_choice'; radio.value = ap.bssid;
    radio.checked = ap.bssid === wifiApDraft; radio.disabled = wifiApBusy;
    radio.onchange = () => { wifiApDraft = ap.bssid; updateWifiApDirty(); renderWifiAp(); };
    const connected = wifiApState && wifiApState.connected && ap.bssid === wifiApState.bssid;
    const saved = wifiApState && ap.bssid === wifiApState.locked_bssid;
    const strength = Number.isFinite(ap.rssi) ? ' · ' + ap.rssi + ' dBm' : '';
    label.append(radio, document.createTextNode(' ' + ap.bssid + strength +
      (connected ? ' · Connected' : '') + (saved ? ' · Saved' : '')));
    list.append(label);
  }
  apNode('wifi_ap_message').textContent = wifiApMessage;
}
async function loadWifiAp() {
  try {
    const response = await fetch('/api/wifi/ap', {cache:'no-store', signal:AbortSignal.timeout(8000)});
    const state = await response.json();
    if (!response.ok || !state.ok) return;
    wifiApState = state;
    apNode('wifi_ap_current').textContent = state.connected
      ? state.ssid + ' · ' + state.rssi + ' dBm · ' + state.bssid
      : state.ssid + ' · Not connected' + (state.recovery ? ' · Recovery Wi-Fi active' : '');
    if (!wifiApDirty && !wifiApBusy) {
      apNode('wifi_ap_mode').value = state.locked_bssid ? 'locked' : 'auto';
      wifiApDraft = state.locked_bssid || '';
    }
    if (wifiApExpected !== null && state.locked_bssid === wifiApExpected && state.connected &&
        (!wifiApExpected || state.bssid === wifiApExpected)) {
      wifiApMessage = 'Saved. Connected successfully.';
      wifiApExpected = null;
    }
    renderWifiAp();
  } catch (_) { /* A reconnect temporarily makes the device unreachable. */ }
}
async function scanWifiAps() {
  if (wifiApBusy) return;
  wifiApBusy = true; wifiApMessage = 'Searching for access points…'; renderWifiAp();
  try {
    let result = await mutationFetch('/api/wifi/scan', '').then(r => r.json());
    if (!result.ok) throw new Error(result.error);
    const started = Date.now();
    do {
      await new Promise(resolve => setTimeout(resolve, 600));
      result = await fetch('/api/wifi/scan', {cache:'no-store', signal:AbortSignal.timeout(8000)}).then(r => r.json());
      if (!result.ok) throw new Error(result.error);
      if (Date.now() - started > 25000) throw new Error('timeout');
    } while (result.scanning);
    wifiApResults = result.aps;
    wifiApMessage = wifiApResults.length ? 'Choose an access point, then save.' : 'No access points found. Try again.';
  } catch (_) { wifiApMessage = 'Could not scan. Wait a moment and try again.'; }
  finally { wifiApBusy = false; renderWifiAp(); }
}
async function saveWifiAp() {
  if (wifiApBusy || !wifiApState) return;
  const bssid = apNode('wifi_ap_mode').value === 'locked' ? wifiApDraft : '';
  if (apNode('wifi_ap_mode').value === 'locked' && !bssid) return;
  wifiApBusy = true; wifiApMessage = 'Saving…'; renderWifiAp();
  try {
    const result = await mutationFetch('/api/wifi/ap', 'bssid=' + encodeURIComponent(bssid)).then(r => r.json());
    if (!result.ok) throw new Error(result.error);
    wifiApState.locked_bssid = bssid;
    wifiApDirty = false; wifiApExpected = bssid;
    wifiApMessage = 'Saved. Reconnecting… If this page stays offline, use the recovery Wi-Fi described above.';
  } catch (_) { wifiApMessage = 'Could not confirm the save. Refresh the page to check the stored setting before retrying.'; }
  finally { wifiApBusy = false; renderWifiAp(); }
}
