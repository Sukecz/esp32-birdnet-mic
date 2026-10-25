// NODE_PATH=tmp/browser-tools/node_modules node tests/recording_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
 const errors=[],writes=[];
 fixture.status.recording={mode:'streaming',state:'ready',supported:true,busy:false,card_ready:false,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:0,dropped_samples:0,file:'',error:''};
 let failStop=false;
 try{
  const page=await browser.newPage({viewport:{width:1280,height:1000}});
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>d.accept());
  await page.route('http://ui.test/**',async route=>{
   const request=route.request(),path=new URL(request.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(request.method()==='POST'){
    assert.equal(request.headers()['x-esp32mic-csrf'],'1');
    const body=new URLSearchParams(request.postData());writes.push(path);
    const r=fixture.status.recording;
    if(path==='/api/device/mode'){r.mode=body.get('mode');r.busy=false;fixture.status.s1_streaming=false;fixture.status.s2_streaming=false;fixture.status.rtsp_server_enabled=r.mode==='streaming';}
    if(path==='/api/recording/settings'){r.duration_minutes=Number(body.get('duration_minutes'));r.segment_minutes=Number(body.get('segment_minutes'));r.auto_start=body.get('auto_start')==='1';}
    if(path==='/api/recording/action'){
     if(body.get('action')==='start'){r.state='recording';r.busy=true;r.card_ready=true;r.free_mib=12000;r.written_kib=420;r.elapsed_seconds=4;r.file='/recordings/01234567-0001.wav.part';}
     else{r.busy=false;r.state=failStop?'error':'saved';r.error=failStop?'SD save failed; partial file preserved':'';if(failStop)return route.fulfill({json:{ok:false,error:r.error}});}
    }
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:'Recording development fixture'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await page.goto('http://ui.test/');await page.waitForFunction(()=>document.getElementById('device_mode_status').textContent.includes('Streaming'));
  await page.selectOption('#device_mode','recording');await page.waitForSelector('#recording_panel:visible');
  await page.fill('#recording_hours','6');assert.equal(await page.locator('#recording_dirty').textContent(),'Unsaved');
  await page.click('#recording_save');await page.waitForFunction(()=>document.getElementById('recording_dirty').textContent==='');
  await page.click('#recording_start');await page.waitForFunction(()=>document.getElementById('device_mode_status').textContent==='● Recording');
  assert(await page.locator('#listen_button').isVisible());assert(await page.locator('#listen_button').isEnabled());
  assert(await page.locator('#recording_start').isDisabled());assert(await page.locator('#recording_hours').isDisabled());
  assert.match(await page.locator('#recording_summary').textContent(),/12000 MiB free/);
  fs.mkdirSync('tmp/recording-development',{recursive:true});
  await page.screenshot({path:'tmp/recording-development/recording-desktop.png',fullPage:true});
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'tmp/recording-development/recording-mobile.png',fullPage:true});
  failStop=true;await page.click('#recording_stop');await page.waitForFunction(()=>document.getElementById('device_mode_status').textContent==='● Error');
  assert.match(await page.locator('#recording_summary').textContent(),/partial file preserved/);
  await page.selectOption('#device_mode','streaming');await page.waitForSelector('#recording_panel',{state:'hidden'});
  assert.equal(errors.length,0,errors.join('\n'));assert(writes.includes('/api/recording/action'));
  console.log('Recording real browser: mode, settings, record/stop faults, Listen availability, mobile layout and CSRF passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
