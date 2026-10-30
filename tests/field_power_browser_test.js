const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});const errors=[];
 try{for(const packed of [false,true]){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.status.recording={mode:'recording',state:'ready',supported:true,busy:false,card_ready:true,cooling_pending:false,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:1000,dropped_samples:0,file:'',error:''};
  fixture.status.field_power={enabled:false,cutoff_mv:3500,confirm_seconds:30,hysteresis_mv:100,wifi_delay_seconds:3,sleep_after_save:true,state:'disabled',latched:false,start_allowed:true,can_acknowledge:false,sample_mv:4000,remaining_seconds:0};
  const page=await browser.newPage({viewport:{width:390,height:844}});let accept=true,gets=0,offAt=0,offFail=true,transient=false;const posts=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>accept?d.accept():d.dismiss());
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');const a=h.indexOf('WEBUI_INDEX_GZ[]');assert(a>=0);source=zlib.gunzipSync(Buffer.from([...h.slice(a,h.indexOf('};',a)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  await page.route('http://power.test/**',route=>{
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(req.method()==='POST'){
    assert.equal(req.headers()['x-esp32mic-csrf'],'1');const body=Object.fromEntries(new URLSearchParams(req.postData()));posts.push({path,body});
    if(path==='/api/field_power')for(const [k,v] of Object.entries(body))fixture.status.field_power[k]=['enabled','sleep_after_save'].includes(k)?v==='1':Number(v);
    if(path==='/api/recording/action'){if(body.action==='wifi_off'){offAt=Date.now()+fixture.status.field_power.wifi_delay_seconds*1000;fixture.status.wifi_off_pending=true;fixture.status.wifi_off_error='';}const r=fixture.status.recording;if(body.action==='start'){r.state='recording';r.busy=true;}if(body.action==='stop'){r.state='saved';r.busy=false;}}
    return route.fulfill({json:{ok:true}});
   }
   ++gets;
   if(path==='/api/status'&&offAt){
    if(transient){transient=false;return route.abort('connectionrefused');}
    if(Date.now()>=offAt){
     if(!offFail)return route.abort('connectionrefused');
     fixture.status.wifi_off_pending=false;fixture.status.wifi_off_error='Could not turn Wi-Fi off. Try again.';offAt=0;
    }
   }
   if(path==='/api/logs')return route.fulfill({body:''});return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await page.goto('http://power.test/');await page.waitForFunction(()=>fieldPowerState!==null);
  assert(await page.locator('#recording_wifi_off').isDisabled());await page.fill('#field_wifi_delay','5');assert.equal(await page.locator('#field_power_dirty').textContent(),'Unsaved');
  await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#field_wifi_delay').inputValue(),'5');await page.fill('#field_wifi_delay','3');assert.equal(await page.locator('#field_power_dirty').textContent(),'');assert.equal(posts.length,0);
  await page.locator('#batterysec > summary').click();await page.check('#field_guard');await page.locator('#battery_protection_advanced > summary').click();await page.fill('#field_cutoff','3.60');await page.fill('#field_confirm','15');await page.fill('#field_hysteresis','0.12');await page.selectOption('#field_sleep','0');
  await page.click('#field_power_save');await page.waitForFunction(()=>!fieldPowerDirty&&!fieldPowerPending);
  assert.deepEqual(posts[0].body,{enabled:'1',cutoff_mv:'3600',confirm_seconds:'15',hysteresis_mv:'120',sleep_after_save:'0'});
  fixture.status.field_power.start_allowed=false;fixture.status.field_power.state='measurement_unavailable';await page.evaluate(()=>loadStatus());assert(await page.locator('#recording_start').isDisabled());
  fixture.status.field_power.start_allowed=true;fixture.status.field_power.state='ready';await page.evaluate(()=>loadStatus());await page.click('#recording_start');await page.waitForFunction(()=>recordingState.state==='recording');
  assert(await page.locator('#recording_wifi_off').isEnabled());assert(await page.locator('#field_guard').isDisabled());assert(await page.locator('#field_wifi_delay').isDisabled());assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  fs.mkdirSync('tmp/field-power',{recursive:true});await page.screenshot({path:`tmp/field-power/mobile-${packed?'packed':'source'}.png`,fullPage:true});
  accept=false;await page.click('#recording_wifi_off');assert(!posts.some(p=>p.body.action==='wifi_off'));accept=true;
  await page.click('#recording_wifi_off');await page.waitForFunction(()=>wifiOffIntent);
  assert(await page.locator('#recording_stop').isDisabled());
  await page.waitForFunction(()=>!wifiOffIntent&&document.getElementById('field_wifi_feedback').textContent.includes('retry'),null,{timeout:12000});
  assert(await page.locator('#recording_wifi_off').isEnabled());assert(await page.locator('#recording_stop').isEnabled());
  assert(await page.locator('#field_guard').isDisabled()); // recording still owns the settings lock.
  offFail=false;transient=true;await page.click('#recording_wifi_off');await page.waitForFunction(()=>wifiOffIntent);
  await page.waitForTimeout(1200);assert(!/connection closed/.test(await page.locator('#field_wifi_feedback').textContent()));
  await page.waitForFunction(()=>document.getElementById('field_wifi_feedback').textContent.includes('connection closed'),null,{timeout:12000});
  assert.match(await page.locator('#field_wifi_feedback').textContent(),/Restart.*reconnect/);
  await page.waitForTimeout(500);const settled=gets;await page.evaluate(()=>loadAll(true));await page.waitForTimeout(3300);assert.equal(gets,settled);assert.equal(posts.filter(p=>p.body.action==='wifi_off').length,2);
  // A partially failed driver stop can reconnect late. The page must recover without a reload.
  offAt=0;fixture.status.wifi_off_pending=false;fixture.status.wifi_off_error='Could not turn Wi-Fi off. Try again.';
  await page.waitForFunction(()=>!wifiOffIntent,null,{timeout:18000});
  assert(await page.locator('#recording_wifi_off').isEnabled());assert(await page.locator('#recording_stop').isEnabled());
  await page.close();
 }assert.deepEqual(errors,[]);console.log('Field power UI source + packed: configurable settings, dirty revert, start guard, busy lock, failed switch-off recovery/retry, transient failures and confirmed disconnect passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
