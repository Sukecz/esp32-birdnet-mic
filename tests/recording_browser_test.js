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
  assert.equal(await page.locator('#listen_volume').getAttribute('max'),'150');
  assert.equal(await page.locator('.health-checks #overview_mic').count(),1);
  for(const width of [1280,390,320]){
   await page.setViewportSize({width,height:1000});
   const mode=await page.locator('#device_mode').boundingBox(),listen=await page.locator('#listen_button').boundingBox(),volume=await page.locator('.listen-volume').boundingBox();
   assert(mode.x<volume.x);assert(volume.x+volume.width<=listen.x);
   assert(Math.abs(mode.y+mode.height/2-listen.y-listen.height/2)<2);
   assert(Math.abs(volume.y+volume.height/2-listen.y-listen.height/2)<2);
   await page.evaluate(()=>listenMessage('Listening stopped.'));
   const message=await page.locator('#listen_status').boundingBox();
   assert(message.y>=listen.y+listen.height);assert(Math.abs(message.x+message.width-listen.x-listen.width)<2);
   await page.evaluate(()=>listenMessage(''));
   const slider=await page.locator('#listen_volume').boundingBox();
   for(const value of [0,9,10,71,99,100,149,150]){
    await page.locator('#listen_volume').evaluate((e,v)=>{e.value=String(v);e.dispatchEvent(new Event('input',{bubbles:true}));},value);
    assert.deepEqual(await page.locator('#listen_volume').boundingBox(),slider);
    assert.deepEqual(await page.locator('#listen_button').boundingBox(),listen);
   }
   await page.mouse.move(slider.x+2,slider.y+slider.height/2);await page.mouse.down();
   await page.mouse.move(slider.x+slider.width-2,slider.y+slider.height/2,{steps:15});await page.mouse.up();
   assert.deepEqual(await page.locator('#listen_volume').boundingBox(),slider);
   assert.deepEqual(await page.locator('#listen_button').boundingBox(),listen);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  }
  await page.setViewportSize({width:1280,height:1000});
  await page.selectOption('#device_mode','recording');await page.waitForSelector('#recording_panel:visible');
  fixture.status.recording.storage='sense_builtin';await page.evaluate(()=>loadStatus());
  assert.match(await page.locator('#recording_storage').textContent(),/Sense built-in microSD slot.*FAT32.*32 GB/);
  assert(await page.locator('#recording_segment').isVisible());
  fixture.status.recording.storage='external_spi';await page.evaluate(()=>loadStatus());
  assert.equal(await page.locator('#recording_storage').textContent(),'External SPI microSD module.');
  assert(await page.locator('#streamssec').isHidden());
  assert(await page.locator('#overview_streams').isHidden());
  assert(await page.locator('#overview_clients').isHidden());
  await page.locator('#healthsec > summary').click();
  assert(await page.locator('#health_capture').isVisible());
  assert(await page.locator('#health_stalls').isHidden());
  await page.locator('#healthsec > summary').click();
  await page.locator('#audiosec > summary').click();
  await page.waitForFunction(()=>document.getElementById('audioadvanced').open);
  assert(await page.locator('#sel_buf').isVisible());
  assert.equal(await page.locator('#t_buf').textContent(),'Audio buffer size');
  await page.locator('#audiosec > summary').click();
  await page.selectOption('#recording_duration_mode','1');await page.fill('#recording_hours','360');assert.equal(await page.locator('#recording_dirty').textContent(),'Unsaved');
  await page.click('#recording_save');await page.waitForFunction(()=>document.getElementById('recording_dirty').textContent==='');
  await page.click('#recording_start');await page.waitForFunction(()=>document.getElementById('device_mode_status').textContent==='Recording: OK');
  assert(await page.locator('#listen_button').isVisible());assert(await page.locator('#listen_button').isEnabled());
  assert(await page.locator('#recording_start').isDisabled());assert(await page.locator('#recording_hours').isDisabled());
  assert.match(await page.locator('#recording_summary').textContent(),/12.6 GB free/);
  fs.mkdirSync('tmp/recording-development',{recursive:true});
  await page.screenshot({path:'tmp/recording-development/recording-desktop.png',fullPage:true});
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'tmp/recording-development/recording-mobile.png',fullPage:true});
  failStop=true;await page.click('#recording_stop');await page.waitForFunction(()=>document.getElementById('device_mode_status').textContent.startsWith('Recording: Error'));
  assert.match(await page.locator('#recording_summary').textContent(),/partial file preserved/);
  await page.selectOption('#device_mode','streaming');await page.waitForSelector('#recording_panel',{state:'hidden'});
  assert(await page.locator('#streamssec').isVisible());
  assert(await page.locator('#overview_streams').isVisible());
  assert(await page.locator('#overview_clients').isVisible());
  assert.equal(errors.length,0,errors.join('\n'));assert(writes.includes('/api/recording/action'));
  console.log('Recording real browser: mode, settings, record/stop faults, Listen availability, mobile layout and CSRF passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
