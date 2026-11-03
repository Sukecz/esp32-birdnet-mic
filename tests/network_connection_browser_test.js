const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 const errors=[];let cases=0;fs.mkdirSync('tmp/network-connection-ui',{recursive:true});
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.status.setup={ok:true,complete:true,default_mode:'streaming',connection:'home',connected:true,ssid:'Saved home',home_ssid:'Current home',ip:'192.0.2.80',ap_active:false,ap_name:'BirdNETMic-TEST'};
  fixture.status.recording={mode:'streaming',supported:true,busy:false,card_ready:false,state:'idle',duration_minutes:0,segment_minutes:60,auto_start:false};
  fixture.status.battery={supported:true,enabled:true,valid:true,voltage:4,percent:80,calibration_percent:0};
  fixture.status.field_power={enabled:false,cutoff_mv:3500,confirm_seconds:30,hysteresis_mv:100,wifi_delay_seconds:3,sleep_after_save:true,state:'disabled',latched:false,start_allowed:true};
  let html=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');html=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const page=await browser.newPage({viewport:{width,height:900}}),writes=[];let failure=false;
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://network.test/**',route=>{
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:html});
   if(path==='/ap-password.js')return require('./ui_assets').apPassword(route,packed);
   if(path==='/api/setup/access')return route.fulfill({json:{ok:true,password:'TEST2345'}});
   if(req.method()==='POST'){
    assert.equal(req.headers()['x-esp32mic-csrf'],'1');const values=Object.fromEntries(new URLSearchParams(req.postData()));writes.push({path,values});
    assert.equal(path,'/api/setup/recording_connection');assert.deepEqual(Object.keys(values),['connection']);
    if(failure)return route.fulfill({status:503,json:{ok:false,error:'Could not save recording connection.'}});
    fixture.status.setup.connection=values.connection;return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:''});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
  });
  await page.goto('http://network.test/');await page.waitForFunction(()=>deviceSetupState&&batteryState&&!pollBusy);
  await page.selectOption('#themeSel',theme);
  assert.equal(await page.locator('details.card[open]').count(),0);
  assert.equal(await page.locator('details:not(.card):not([open])').count(),0);
  for(const id of ['battery_monitor','recording_auto','recording_log_sd','field_guard'])assert.equal(await page.locator('#'+id).evaluate(e=>e.tagName),'SELECT');
  await page.evaluate(()=>openSection('networksec'));
  assert.match(await page.locator('#home_wifi_status').textContent(),/Connected.*192.0.2.80/);
  assert.equal(await page.locator('#home_wifi_network').textContent(),'Current home');
  assert.match(await page.locator('#device_wifi_status').textContent(),/^Off/);
  assert(await page.locator('#home_wifi_status').evaluate(e=>!!(e.compareDocumentPosition(document.getElementById('direct_access'))&Node.DOCUMENT_POSITION_FOLLOWING)));
  const input=page.locator('#recording_connection');
  const settled=()=>page.waitForFunction(()=>!sectionBusy.size&&!pollBusy);
  const original=await input.inputValue();await input.selectOption('direct');
  assert(await input.evaluate(e=>e.classList.contains('dirty')));
  assert.match(await input.locator('..').textContent(),/Unsaved/);
  assert.match(await input.evaluate(e=>getComputedStyle(e).borderColor),/rgb\((217, 119, 6|245, 158, 11)\)/);
  await page.evaluate(()=>loadStatus());assert.equal(await input.inputValue(),'direct');
  await input.selectOption(original);assert(!await input.evaluate(e=>e.classList.contains('dirty')));assert.equal(writes.length,0);
  await input.selectOption('direct');failure=true;await page.click('#recording_connection_apply');await settled();
  assert(await input.evaluate(e=>e.classList.contains('dirty')));assert.match(await page.locator('#recording_connection_feedback').textContent(),/Could not save/);
  failure=false;await page.locator('#networksec>.discard').click();await settled();assert.equal(await input.inputValue(),'home');
  // Saving this preference must preserve drafts in another Network setting.
  await page.fill('#in_mdns_host','unsaved-host');await input.selectOption('direct');await page.click('#recording_connection_apply');await settled();
  assert.equal(await input.inputValue(),'direct');assert(!await input.evaluate(e=>e.classList.contains('dirty')));
  assert.equal(await page.locator('#in_mdns_host').inputValue(),'unsaved-host');assert(await page.locator('#in_mdns_host').evaluate(e=>e.classList.contains('dirty')));
  assert.equal(fixture.status.setup.default_mode,'streaming');
  // The saved Recording preference must not be presented as the current Wi-Fi state.
  assert.match(await page.locator('#home_wifi_status').textContent(),/^Connected/);assert.match(await page.locator('#device_wifi_status').textContent(),/^Off/);
  fixture.status.recording.mode='recording';fixture.status.setup.ap_active=true;await page.evaluate(()=>loadStatus());
  assert.match(await page.locator('#home_wifi_status').textContent(),/^Connected/);assert.match(await page.locator('#device_wifi_status').textContent(),/Active.*Standalone/);
  fixture.status.setup.connected=false;await page.evaluate(()=>loadStatus());
  assert.equal(await page.locator('#home_wifi_status').textContent(),'Not connected');assert.equal(await page.locator('#home_wifi_network_row').isVisible(),false);
  fixture.status.setup.connection='home';await page.locator('#networksec>.discard').click();await settled();
  assert.match(await page.locator('#device_wifi_status').textContent(),/Home Wi-Fi unavailable/);
  fixture.status.setup.connected=true;await page.evaluate(()=>loadStatus());assert.match(await page.locator('#device_wifi_status').textContent(),/Backup/);
  assert.equal(await page.locator('#recording_wifi_off').textContent(),'Turn Wi-Fi off until restart');
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  if(!packed&&[320,1280].includes(width))await page.locator('#networksec').screenshot({path:`tmp/network-connection-ui/network-${width}-${theme}.png`});
  await page.evaluate(()=>openSection('batterysec'));assert(await page.locator('#battery_calibration').isVisible());
  assert.equal(await page.locator('#battery_save').textContent(),'Apply');
  const sizes=await page.evaluate(()=>['battery_monitor','recording_connection','sel_mdns'].map(id=>getComputedStyle(document.getElementById(id)).fontSize));assert.equal(new Set(sizes).size,1);
  if(!packed&&[320,1280].includes(width))await page.locator('#batterysec').screenshot({path:`tmp/network-connection-ui/battery-${width}-${theme}.png`});
  cases++;await page.close();
 }
 assert.deepEqual(errors,[]);console.log(`Network: ${cases} source/embedded/theme/viewport scenarios; scoped save/drafts, actual versus preferred connections, common controls and visible calibration passed`);
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
