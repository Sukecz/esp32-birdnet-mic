// NODE_PATH=tmp/browser-tools/node_modules node tests/time_sync_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
function html(packed){
 if(!packed)return fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
 const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');
 const a=h.indexOf('WEBUI_INDEX_GZ[]');
 return zlib.gunzipSync(Buffer.from([...h.slice(a,h.indexOf('};',a)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
}
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const errors=[];fs.mkdirSync('tmp/time-sync-fix',{recursive:true});
 try{
  for(const packed of [false,true]){
   const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
   Object.assign(fixture.status,{fw_version:'1.36',time_sync_enabled:true,time_valid:false,time_synced:false,time_sync_status:'waiting_wifi',last_time_sync:'never',local_time:'unavailable',utc_time:'unavailable'});
   const page=await browser.newPage({viewport:{width:320,height:900}});page.on('pageerror',e=>errors.push(e.message));
   await page.route('http://time.test/**',route=>{
    const r=route.request(),path=new URL(r.url()).pathname;
    if(path==='/')return route.fulfill({contentType:'text/html',body:html(packed)});
    if(path==='/api/action/time_sync'){
     assert.equal(r.method(),'POST');assert.equal(r.headers()['x-esp32mic-csrf'],'1');
     return route.fulfill({json:{ok:true,pending:true}});
    }
    if(path==='/api/logs')return route.fulfill({body:''});
    return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
   });
   await page.goto('http://time.test/');await page.waitForFunction(()=>document.getElementById('time_sync').textContent==='Waiting for Wi-Fi');
   await page.locator('#timesec').evaluate(e=>e.open=true);
   assert.match(await page.locator('#local_time').textContent(),/Unavailable/);
   fixture.status.time_sync_status='syncing';await page.evaluate(()=>loadStatus());
   assert.equal(await page.locator('#time_sync').textContent(),'Waiting for NTP');
   // A retained clock must remain visible while fresh NTP confirmation is pending.
   Object.assign(fixture.status,{time_valid:true,local_time:'2026-10-28 10:00:00',utc_time:'2026-10-28 09:00:00',effective_time_offset_min:60});
   await page.evaluate(()=>loadStatus());assert.match(await page.locator('#local_time').textContent(),/10:00:00 \(UTC\+01:00\)/);
   assert.match(await page.locator('#utc_time').textContent(),/09:00:00/);
   await page.click('#b_sync_now');await page.waitForFunction(()=>document.querySelector('#timesec > .feedback')?.textContent.includes('Synchronization requested'));
   assert.equal(await page.locator('#time_sync').textContent(),'Waiting for NTP');
   Object.assign(fixture.status,{time_synced:true,time_sync_status:'synced',last_time_sync:'2s ago'});
   await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#time_sync').textContent(),'YES');
   assert.doesNotMatch(await page.locator('#timesec > .feedback').textContent(),/Waiting/);
   assert.equal(await page.locator('#last_sync').textContent(),'2s ago');
   for(const width of [320,390,1280])for(const theme of ['light','dark']){
    await page.setViewportSize({width,height:900});await page.evaluate(t=>document.documentElement.dataset.theme=t,theme);
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
    if(!packed&&width===320)await page.screenshot({path:`tmp/time-sync-fix/synced-${theme}.png`,fullPage:true});
   }
   Object.assign(fixture.status,{time_sync_enabled:false,time_sync_status:'disabled'});await page.evaluate(()=>loadStatus());
   assert.equal(await page.locator('#time_sync').textContent(),'OFF');assert(await page.locator('#b_sync_now').isDisabled());
   assert.match(await page.locator('#local_time').textContent(),/10:00:00/);
   Object.assign(fixture.status,{time_valid:false,time_synced:false,local_time:'unavailable',utc_time:'unavailable'});await page.evaluate(()=>loadStatus());
   assert.match(await page.locator('#local_time').textContent(),/Unavailable/);
   await page.close();
  }
  assert.deepEqual(errors,[]);console.log('Time UI: waiting Wi-Fi/NTP, retained clock, queued manual sync, confirmed sync, disabled/offline, source and embedded assets, mobile/desktop themes passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
