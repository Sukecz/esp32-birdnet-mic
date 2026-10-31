const assert=require('node:assert/strict');
const fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 try{
 for(const packed of [false,true]){
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');source=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json','utf8'));
  let board='c6',failure=false,profileOffline=false;
  const writes=[],errors=[];
  const page=await browser.newPage({viewport:{width:390,height:844}});
  page.on('pageerror',e=>errors.push(e.message));
  const profile=format=>({ok:true,board:'XIAO ESP32-'+board.toUpperCase(),mic_format:format,current_format:fixture.audio_status.mic_format,sample_rate:48000,gain:format===2?24:1.5,buffer_size:512,i2s_shift:12,hp_enable:true,hp_cutoff_hz:500,cpu_mhz:format===2&&board!=='s3'?160:0});
  await page.route('http://ui.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const req=route.request(),url=new URL(req.url()),path=url.pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(path==='/api/microphone_profile'){
    if(profileOffline)return route.abort();
    return route.fulfill({json:profile(Number(url.searchParams.get('format')))});
   }
   if(req.method()!=='GET'){
    const values=Object.fromEntries(new URLSearchParams(req.postData()));writes.push({path,values});
    if(failure)return route.fulfill({json:{ok:false,error:'i2s_restart_failed'}});
    if(path==='/api/settings/microphone'){
     const format=Number(values.format),a=fixture.audio_status;
     if(values.settings==='recommended'){
      const p=profile(format);Object.assign(a,{sample_rate:p.sample_rate,gain:p.gain,buffer_size:p.buffer_size,hp_enable:p.hp_enable,hp_cutoff_hz:p.hp_cutoff_hz});
      if(format!==2)a.i2s_shift=p.i2s_shift;
      if(p.cpu_mhz)fixture.thermal.cpu_mhz=p.cpu_mhz;
     }else if(format===2)a.sample_rate=48000;
     a.mic_format=format;
    }
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:'Example log'});
   if(path==='/api/thermal')fixture.thermal.cpu_max_mhz=['s3','c5'].includes(board)?240:160;
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.30'}});
  });
  await page.goto('http://ui.test/');await page.waitForFunction(()=>document.querySelector('#in_gain').value==='1.50');
  await page.evaluate(()=>openSection('audiosec'));
  assert.equal(await page.locator('#sel_mic_format option').count(),3); // ICS/INMP remain one model group.
  // Merely selecting a microphone, cancelling or pressing Escape sends no writes.
  await page.selectOption('#sel_mic_format','2');await page.locator('#btn_mic_apply').click();
  await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
  assert(await page.locator('#mic_dialog_title').evaluate(e=>e.getBoundingClientRect().top>=e.closest('dialog').getBoundingClientRect().top));
  assert.equal(writes.length,0);assert.match(await page.locator('#mic_dialog_target').textContent(),/C6.*IM73D122/);
  assert.match(await page.locator('#mic_dialog_rows').textContent(),/24/);
  await page.keyboard.press('Escape');await page.waitForFunction(()=>!document.querySelector('#mic_dialog').open);
  assert.equal(writes.length,0);assert.equal(await page.locator('#sel_mic_format').inputValue(),'2');
  await page.locator('#btn_mic_apply').click();await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
  await page.locator('#btn_mic_cancel').click();assert.equal(writes.length,0);
  // Actual dialog defaults are keyboard accessible and fit a 320 px screen.
  await page.locator('#in_gain').fill('7');await page.locator('#btn_mic_apply').click();
  await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
  assert.match(await page.locator('#mic_dialog_drafts').textContent(),/replaces unsaved/);
  for(const width of [320,390,1280]){
   await page.setViewportSize({width,height:844});
   assert(await page.locator('#mic_dialog').evaluate(e=>e.scrollWidth<=e.clientWidth));
   assert(await page.locator('#mic_dialog').evaluate(e=>{const b=e.getBoundingClientRect();return b.x>=0&&b.right<=innerWidth;}));
  }
  await page.locator('#mic_dialog').screenshot({path:'tmp/microphone-profile-dialog.png'});
  await page.locator('#btn_mic_use').click();await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent==='Microphone and recommended settings saved.');
  assert.equal(writes.length,1);assert.deepEqual(writes[0],{path:'/api/settings/microphone',values:{format:'2',settings:'recommended'}});
  await page.waitForFunction(()=>document.querySelector('#in_gain').value==='24.00');
  assert.equal(fixture.audio_status.gain,24);assert.equal(await page.locator('#in_gain').inputValue(),'24.00');
  assert(await page.locator('#in_shift').isDisabled());assert.equal(fixture.thermal.cpu_mhz,160);
  // Applying the current microphone does not prompt. The explicit button still does.
  await page.locator('#btn_mic_apply').click();assert(!await page.locator('#mic_dialog').evaluate(e=>e.open));
  await page.locator('#btn_mic_recommended').click();await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
  await page.locator('#btn_mic_cancel').click();
  // Keep uses stored custom values, one request, without discarding other pending edits.
  Object.assign(fixture.audio_status,{mic_format:0,gain:2.75,buffer_size:2048,sample_rate:96000,hp_cutoff_hz:1300});
  fixture.thermal.cpu_mhz=80;
  await page.evaluate(()=>refreshAll());await page.locator('#in_gain').fill('9');
  await page.selectOption('#sel_mic_format','2');await page.locator('#btn_mic_apply').click();
  await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);await page.locator('#btn_mic_keep').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent==='Microphone saved. Custom values preserved.');
  assert.equal(fixture.audio_status.gain,2.75);assert.equal(fixture.audio_status.buffer_size,2048);assert.equal(fixture.thermal.cpu_mhz,80);
  assert.equal(fixture.audio_status.sample_rate,48000);assert.equal(await page.locator('#in_gain').inputValue(),'9');
  // Rejecting capture restart keeps drafts, and no partial successful state is shown.
  failure=true;await page.selectOption('#sel_mic_format','1');await page.locator('#btn_mic_apply').click();
  await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);await page.locator('#btn_mic_use').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent.includes('could not restart'));
  assert.equal(fixture.audio_status.mic_format,2);assert.equal(await page.locator('#sel_mic_format').inputValue(),'1');
  assert(await page.locator('#sel_mic_format').evaluate(e=>e.classList.contains('dirty')));
  failure=false;await page.locator('#btn_mic_apply').click();await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
  await page.locator('#btn_mic_use').click();await page.waitForFunction(()=>document.querySelector('#in_gain').value==='1.50');
  assert.equal(fixture.audio_status.mic_format,1);assert(await page.locator('#in_shift').isEnabled());
  // A failed recommendation lookup never opens a misleading dialog or writes anything.
  profileOffline=true;let count=writes.length;await page.locator('#btn_mic_recommended').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent.includes('Could not load'));
  assert.equal(writes.length,count);assert(!await page.locator('#mic_dialog').evaluate(e=>e.open));profileOffline=false;
  // Every board/microphone combination; 240 MHz is visible only on S3/C5.
  for(board of ['c3','s3','c5','c6']){
   fixture.status.board_id='xiao-esp32'+board;fixture.thermal.cpu_mhz=['s3','c5'].includes(board)?240:160;
   await page.evaluate(()=>refreshAll());
   assert.equal(await page.locator('#sel_cpu option[value="240"]').count(),['s3','c5'].includes(board)?1:0);
   if(['s3','c5'].includes(board))assert.equal(await page.locator('#sel_cpu').inputValue(),'240');
   for(const format of ['0','1','2']){
    await page.selectOption('#sel_mic_format',format);await page.locator('#btn_mic_recommended').click();
    await page.waitForFunction(()=>document.querySelector('#mic_dialog').open);
    assert.match(await page.locator('#mic_dialog_target').textContent(),new RegExp(board.toUpperCase()));
    const rows=await page.locator('#mic_dialog_rows').textContent();
    assert.equal(rows.includes('CPU frequency'),format==='2'&&board!=='s3');
    await page.locator('#btn_mic_cancel').click();
   }
  }
  assert.deepEqual(errors,[]);await page.close();console.log((packed?'Embedded':'Source')+' microphone UI: cancel/Escape, atomic apply/keep, failed saves, retained drafts, all board profiles, responsive dialog and conditional 240 MHz passed');
 }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
