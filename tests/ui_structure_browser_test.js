// NODE_PATH=tmp/browser-tools/node_modules CHROMIUM=/path/to/chrome node tests/ui_structure_browser_test.js
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {chromium}=require('playwright-core');
const fixture=require('./fixtures/ui-state.json');
(async()=>{
 const source=fs.readFileSync(process.env.WEBUI_HTML||'esp32-birdnet-mic/webui/index.html','utf8');
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const errors=[],writes=[];
 try{
  const page=await browser.newPage({viewport:{width:1280,height:900},hasTouch:true});
  page.on('pageerror',e=>errors.push(e.message));
  page.on('dialog',dialog=>dialog.accept());
  let failAudio=false;
  await page.route('http://ui.test/**',async route=>{
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(req.method()!=='GET'){writes.push(path);return route.fulfill({json:{ok:true}});}
   if(path==='/api/audio_status'&&failAudio)return route.abort();
   if(path==='/api/wifi/ap/scan')return route.fulfill({json:{ok:true,scanning:false,aps:[{bssid:'02:00:00:00:00:01',rssi:-60},{bssid:'02:00:00:00:00:02',rssi:-70}]}});
   if(path==='/api/logs')return route.fulfill({body:'Example device log'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.27'}});
  });
  const load=async()=>{await page.goto('http://ui.test/');await page.waitForFunction(()=>document.querySelector('#overview_mic').textContent==='Microphone: OK');};
  await load();
  const sections=page.locator('details.card');
  assert.equal(await sections.count(),10);
  assert.equal(await page.locator('details.card[open]').count(),0);
  assert(await page.locator('#listen_button').isVisible());
  assert.equal(await page.locator('#listen_volume_value').textContent(),'60%');
  await page.locator('#listen_volume').focus();await page.keyboard.press('End');
  assert.equal(await page.locator('#listen_volume').inputValue(),'150');
  assert.equal(await page.locator('#listen_volume_value').textContent(),'150%');
  await page.keyboard.press('Home');assert.equal(await page.locator('#listen_volume_value').textContent(),'0%');
  for(const [id,section] of Object.entries({sel_max_clients:'streamssec',in_shift:'audiosec',sel_tx:'networksec',sel_cpu:'thermalsec',in_chk:'recoverysec',b_sync_now:'timesec',b_defaults:'recoverysec'}))
   assert.equal(await page.locator('#'+id).evaluate(el=>el.closest('details.card').id),section);
  await page.locator('#audiosec>summary').click();await page.locator('#healthsec>summary').click();
  await page.locator('#in_gain').fill('2.75');
  await page.evaluate(()=>refreshAll());
  assert.equal(await page.locator('details.card[open]').count(),2);
  assert.equal(await page.locator('#in_gain').inputValue(),'2.75');
  fixture.audio_status.mic_format=2;await page.evaluate(()=>refreshAll());
  for(const id of ['in_rate','btn_rate_set','in_shift','btn_shift_set'])assert(await page.locator('#'+id).isDisabled());
  for(const id of ['in_gain','sel_hp','in_hp_cutoff'])assert(await page.locator('#'+id).isEnabled());
  fixture.audio_status.mic_format=0;await page.evaluate(()=>refreshAll());
  assert(await page.locator('#in_shift').isEnabled());
  await page.evaluate(()=>{wifiApDirty=false;wifiApResults=[{bssid:'02:00:00:00:00:01',rssi:-60},{bssid:'02:00:00:00:00:02',rssi:-70}];wifiApDraft='02:00:00:00:00:01';document.querySelector('#wifi_ap_mode').value='locked';wifiApModeChanged();});
  await page.evaluate(()=>openSection('networksec'));
  assert(await page.locator('#wifi_ap_results input[type=radio]').first().evaluate(el=>{const w=el.getBoundingClientRect().width;return w>0&&w<30;}));
  // Historical totals stay informational; microphone warning remains visible with cards closed.
  fixture.audio_status.dma_overruns_total=9;fixture.audio_status.dma_overruns_1min=0;
  fixture.audio_status.mic_signal='flatline';fixture.audio_status.mic_flatline_sec=25;
  await page.evaluate(()=>{document.querySelectorAll('details').forEach(el=>el.open=false);return refreshAll();});
  assert(await page.locator('#microphone_notice').isVisible());
  assert.equal(await page.locator('#health_dma').getAttribute('class'),'v');
  await page.locator('#microphone_notice a').first().click();assert(await page.locator('#in_gain').isVisible());
  fixture.thermal.notice=true;fixture.thermal.latched_persist=true;fixture.thermal.can_acknowledge=false;
  await page.evaluate(()=>refreshAll());assert(await page.locator('#thermal_notice').isVisible());
  assert(await page.locator('#btn_thermal_notice_ack').isDisabled());
  await page.locator('#thermal_notice a').click();assert(await page.locator('#sel_cpu').isVisible());
  fixture.audio_status.mic_signal='ok';fixture.thermal.notice=false;fixture.thermal.latched_persist=false;
  failAudio=true;await page.evaluate(()=>refreshAll());assert.match(await page.locator('#connection_status').textContent(),/out of date/);
  failAudio=false;await page.evaluate(()=>refreshAll());assert.equal(await page.locator('#connection_status').textContent(),'');
  for(const width of [320,390,768,1280]){
   await page.setViewportSize({width,height:900});
   await page.evaluate(()=>{document.querySelectorAll('details').forEach(el=>el.open=true);});
   const overflow=await page.evaluate(()=>({viewport:innerWidth,width:document.documentElement.scrollWidth}));
   assert(overflow.width<=width,JSON.stringify(overflow));
   if(width<=600){
    const settingWidths=await page.locator('tr.setting-row').evaluateAll(rows=>rows.filter(row=>row.getBoundingClientRect().width>0).map(row=>({row:row.getBoundingClientRect().width,table:row.closest('table').getBoundingClientRect().width})));
    assert(settingWidths.every(size=>size.row>=size.table-2),JSON.stringify(settingWidths));
   }
   for(const theme of ['light','dark']){
    await page.selectOption('#themeSel',theme);
    await page.screenshot({path:`tmp/ui-${width}-${theme}-expanded.png`,fullPage:true});
   }
   await page.evaluate(()=>{document.querySelectorAll('details').forEach(el=>el.open=false);});
   await page.locator('#listen_volume').scrollIntoViewIfNeeded();
   const slider=await page.locator('#listen_volume').boundingBox();
   await page.mouse.click(slider.x+slider.width-1,slider.y+slider.height/2);
   assert.equal(await page.locator('#listen_volume').inputValue(),'150');
   await page.touchscreen.tap(slider.x+1,slider.y+slider.height/2);
   assert.equal(await page.locator('#listen_volume').inputValue(),'0');
   await page.touchscreen.tap(slider.x+slider.width-1,slider.y+slider.height/2);
   assert.equal(await page.locator('#listen_volume').inputValue(),'150');
   await page.screenshot({path:`tmp/ui-${width}-collapsed.png`,fullPage:true});
   if(width===390){
    await page.locator('#audiosec>summary').click();
    await page.locator('#audiosec').screenshot({path:'tmp/ui-mobile-audio.png'});
    await page.locator('#audiosec>summary').click();
   }
  }
  await load();assert.equal(await page.locator('details.card[open]').count(),0);
  assert.deepEqual(writes,[]);assert.deepEqual(errors,[]);
  console.log('UI structure: ten closed sections, relocated controls, 0–150% volume, polling/drafts, PDM, alerts, stale data, themes and 320–1280px layouts passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
