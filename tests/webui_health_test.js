const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../esp32-birdnet-mic/webui/index.html'), 'utf8');
for (const match of html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)) {
  new vm.Script(match[1]);
}
const nodes = Object.fromEntries(['health_stalls', 'health_failures', 'health_clients', 'health_buffer', 'health_stop', 'mqtt_discovery_status', 'mqtt_err', 'mqtt_conn'].map(id => [id, {}]));
const context = vm.createContext({$: id => nodes[id] || null, Date});
function loadFunction(name, next) {
  const start = html.indexOf('function ' + name + '(');
  const end = html.indexOf('function ' + next + '(', start);
  assert(start >= 0 && end > start);
  vm.runInContext(html.slice(start, end), context);
}
loadFunction('updateStreamHealth', 'loadAudio');
loadFunction('loadMqtt', 'loadStatus');
context.updateStreamHealth({rtsp_write_stalls: 200, rtsp_write_failures: 3, rtsp_write_timeouts: 2,
  rtsp_client_teardowns: 4, rtsp_client_disconnects: 1, rb_drops: 10, rb_flushes: 8,
  last_stop_stream: 2, last_stop_reason: '<untrusted>', last_stop_time: '2026-09-08 12:00:00', last_stop_age: '1m ago'});
assert.equal(nodes.health_failures.textContent, '3 / 2');
assert.equal(nodes.health_clients.textContent, '4 / 1');
assert(nodes.health_stop.textContent.includes('Stream 2 — <untrusted>'));
assert.equal(nodes.health_stop.innerHTML, undefined);
context.updateStreamHealth({});
assert.equal(nodes.health_stop.textContent, 'No stop recorded');
context.loadMqtt({mqtt_connected: true, mqtt_discovery_status: 'pending / retrying', mqtt_last_error: 'discovery_publish_failed'});
assert.equal(nodes.mqtt_discovery_status.textContent, 'pending / retrying');
assert.equal(nodes.mqtt_err.textContent, 'discovery_publish_failed');
context.loadMqtt({mqtt_connected: true, mqtt_discovery_status: 'published', mqtt_last_error: 'ok'});
assert.equal(nodes.mqtt_discovery_status.textContent, 'published');
assert.equal(nodes.mqtt_err.textContent, 'ok');
console.log('Web UI health and MQTT rendering checks passed');
