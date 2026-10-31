const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});const errors=[];
 try{for(const packed of [false,true]){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.status.recording={mode:'recording',state:'saved',supported:true,busy:false,card_ready:true,cooling_pending:false,elapsed_seconds:60,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:1000,free_mib:1000,dropped_samples:0,file:'test.wav',error:'',logging:{sd_enabled:true,detailed:false,max_kib:1024,file:'/recording.log',bytes:12345,error:''}};
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8'),a=h.indexOf('WEBUI_INDEX_GZ[]');source=zlib.gunzipSync(Buffer.from([...h.slice(a,h.indexOf('};',a)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const page=await browser.newPage({viewport:{width:390,height:900}});page.on('pageerror',e=>errors.push(e.message));let posts=[],reads=0;
  await page.route('http://logging.test/**',route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(req.method()==='POST'){
    assert.equal(req.headers()['x-esp32mic-csrf'],'1');const v=Object.fromEntries(new URLSearchParams(req.postData()));posts.push({path,v});
    if(path==='/api/recording/log/settings')fixture.status.recording.logging={...fixture.status.recording.logging,sd_enabled:v.sd_enabled==='1',detailed:v.detailed==='1',max_kib:Number(v.max_kib)};
    if(path==='/api/recording/log/read')reads=0;
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/recording/log'){if(++reads<2)return route.fulfill({status:202,body:'Reading...'});return route.fulfill({contentType:'text/plain',headers:{'X-ESP32MIC-Log-Bytes':'12345'},body:'[time] Recording: started\n[time] Recording: saved\n<script>bad()</script>'});}
   if(path==='/api/logs')return route.fulfill({body:'[time] Device ready'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await page.goto('http://logging.test/');await page.waitForFunction(()=>recordingLogState!==null);await page.locator('#logssec > summary').click();await page.locator('#recording_log_settings > summary').click();
  assert.equal(await page.locator('#recording_log_level').inputValue(),'0');assert(await page.locator('#recording_log_sd').isChecked());
  await page.selectOption('#recording_log_level','1');assert.equal(await page.locator('#recording_log_dirty').textContent(),'Unsaved');await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#recording_log_level').inputValue(),'1');
  await page.selectOption('#recording_log_level','0');assert.equal(await page.locator('#recording_log_dirty').textContent(),'');assert.equal(posts.length,0);
  await page.selectOption('#recording_log_level','1');await page.fill('#recording_log_max','0.07');await page.click('#recording_log_save');await page.waitForFunction(()=>!recordingLogDirty);
  assert.deepEqual(posts[0].v,{sd_enabled:'1',detailed:'1',max_kib:'68'});
  assert.equal(await page.locator('#recording_log_tail').count(),0);assert.equal(await page.locator('#logssec #recording_log_settings').count(),1);
  fixture.status.recording.busy=true;fixture.status.recording.state='recording';await page.evaluate(()=>loadStatus());
  for(const id of ['recording_log_level','recording_log_sd','recording_log_max'])assert(await page.locator('#'+id).isDisabled());
  fixture.status.recording.busy=false;fixture.status.recording.mode='streaming';await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#recording_log_settings').isVisible(),false);assert.equal(await page.locator('#recording_panel').isVisible(),false);assert.equal(await page.locator('#field_guard').isVisible(),false);assert(await page.locator('#logssec').isVisible());
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  fs.mkdirSync('tmp/standard-logging',{recursive:true});await page.screenshot({path:`tmp/standard-logging/settings-${packed?'packed':'source'}.png`,fullPage:true});await page.close();
 }assert.deepEqual(errors,[]);console.log('Standard logging UI source/packed: normal defaults, edit/revert, save, single common log section and busy lock passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
