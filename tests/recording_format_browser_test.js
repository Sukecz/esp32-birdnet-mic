const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core'),assets=require('./ui_assets');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.status.recording={format:'wav',mode:'recording',state:'ready',supported:true,busy:false,card_ready:true,space_seconds:7200,elapsed_seconds:0,written_kib:0,free_mib:12000,cooling_pending:false,duration_minutes:60,segment_minutes:60,auto_start:false,logging:{sd_enabled:true,detailed:false,max_kib:1024,bytes:0,file:'/recording.log',error:''}};
  fixture.status.field_power={enabled:false,cutoff_mv:3500,confirm_seconds:30,hysteresis_mv:100,wifi_delay_seconds:3,sleep_after_save:true,state:'disabled'};
  let fail=false,writes=0;const errors=[];
  const page=await browser.newPage({viewport:{width,height:900},colorScheme:theme});page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(t=>localStorage.setItem('theme',t),theme);
  await page.route('http://format.test/**',route=>{
   const r=route.request(),path=new URL(r.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:assets.main(packed)});
   if(path==='/ap-password.js')return assets.apPassword(route,packed);
   if(path==='/api/recording/settings'){
    ++writes;if(fail)return route.fulfill({status:503,json:{ok:false,error:'Storage unavailable'}});
    const v=new URLSearchParams(r.postData());Object.assign(fixture.status.recording,{format:v.get('format'),duration_minutes:Number(v.get('duration_minutes')),segment_minutes:Number(v.get('segment_minutes')),auto_start:v.get('auto_start')==='1'});return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:''});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true}});
  });
  await page.goto('http://format.test/');await page.waitForFunction(()=>recordingState&&fieldPowerState&&batteryState);
  const field=page.locator('#recording_format');await field.waitFor({state:'visible'});
  assert.equal(await field.inputValue(),'wav');await field.selectOption('flac');
  assert(await field.evaluate(e=>e.classList.contains('dirty')));
  assert.equal(await field.locator('..').locator('.edit-note').textContent(),'Unsaved');assert(!await page.locator('#recording_hours').evaluate(e=>e.classList.contains('dirty')));assert.equal(await page.locator('#recording_duration_mode').locator('..').locator('.edit-note').textContent(),'');assert(await page.locator('#recording_duration_apply').isDisabled());
  const style=await field.evaluate(e=>({font:getComputedStyle(e).fontSize,other:getComputedStyle(document.getElementById('recording_auto')).fontSize,border:getComputedStyle(e).borderColor}));assert.equal(style.font,style.other);assert(style.border.includes('249')||style.border.includes('245'));
  await page.evaluate(()=>loadStatus());assert.equal(await field.inputValue(),'flac');
  await field.selectOption('wav');assert(!await field.evaluate(e=>e.classList.contains('dirty')));assert.equal(writes,0);
  await field.selectOption('flac');fail=true;await page.evaluate(()=>saveRecordingSettings('format'));assert.equal(await field.inputValue(),'flac');assert(await field.evaluate(e=>e.classList.contains('dirty')));
  fail=false;await page.evaluate(()=>saveRecordingSettings('format'));assert.equal(fixture.status.recording.format,'flac');assert(!await field.evaluate(e=>e.classList.contains('dirty')));
  await field.selectOption('wav');await page.locator('#recording_panel > .discard').click();assert.equal(await field.inputValue(),'flac');
  const layout=await page.evaluate(()=>{
   const e=id=>document.getElementById(id),box=id=>e(id).getBoundingClientRect(),style=id=>getComputedStyle(e(id));
   return {start:box('recording_start').toJSON(),stop:box('recording_stop').toJSON(),check:box('recording_check').toJSON(),
    green:style('recording_start').backgroundColor,red:style('recording_stop').backgroundColor,
    font:style('recording_format_apply').fontSize,otherFont:style('recording_start').fontSize,
    sticky:getComputedStyle(e('recording_format_apply').parentElement).position};
  });
  assert.equal(layout.start.y,layout.stop.y);assert(layout.check.y>=layout.start.y+layout.start.height);
  assert.match(layout.green,/16, 185, 129/);assert.match(layout.red,/239, 68, 68/);assert.equal(layout.font,layout.otherFont);assert.equal(layout.sticky,'static');
  assert(await page.locator('#recording_indicator').isHidden());
  fixture.status.recording.state='recording';fixture.status.recording.busy=true;fixture.status.recording.elapsed_seconds=3661;await page.evaluate(()=>loadStatus());
  assert(await page.locator('#recording_indicator').isVisible());assert.match(await page.locator('#recording_clock').textContent(),/^01:01:0[12]$/);
  fs.mkdirSync('tmp/recording-flat-review',{recursive:true});await page.locator('.top').screenshot({path:`tmp/recording-flat-review/${packed?'embedded':'source'}-${width}-${theme}-rec.png`});
  const rec=await page.evaluate(()=>{const e=document.getElementById('recording_indicator'),m=document.getElementById('device_mode');return {parent:e.parentElement.className,font:getComputedStyle(e).fontSize,modeFont:getComputedStyle(m).fontSize,bg:getComputedStyle(e).backgroundColor,r:e.getBoundingClientRect().toJSON(),m:m.getBoundingClientRect().toJSON()};});
  assert.equal(rec.parent,'mode-controls');assert.equal(rec.font,rec.modeFont);assert.equal(rec.bg,'rgba(0, 0, 0, 0)');assert(rec.r.x>=rec.m.x+rec.m.width);assert(Math.abs(rec.r.y+rec.r.height/2-rec.m.y-rec.m.height/2)<3);
  const dot=await page.locator('.recording-dot').evaluate(e=>getComputedStyle(e).backgroundColor);assert.equal(dot,'rgb(239, 68, 68)');
  await page.evaluate(()=>{recordingClockSnapshot.at-=2200;updateRecordingClock();});assert.match(await page.locator('#recording_clock').textContent(),/^01:01:0[34]$/);
  await page.evaluate(()=>{recordingClockSnapshot.at-=20000;updateRecordingClock();});const frozen=await page.locator('#recording_clock').textContent();await page.evaluate(()=>updateRecordingClock());assert.equal(await page.locator('#recording_clock').textContent(),frozen);
  for(const state of ['saving','cooling','error','saved','ready']){fixture.status.recording.state=state;fixture.status.recording.busy=false;await page.evaluate(()=>loadStatus());assert(await page.locator('#recording_indicator').isHidden());}
  await page.fill('#recording_hours','75');await field.selectOption('wav');await page.evaluate(()=>loadStatus());
  assert.equal(await page.inputValue('#recording_hours'),'75');assert(await page.locator('#recording_hours').evaluate(e=>e.classList.contains('dirty')));
  fail=true;await page.locator('#recording_format_apply').click();await page.waitForFunction(()=>document.getElementById('recording_format_feedback').textContent.includes('Storage unavailable'));
  assert.equal(await page.inputValue('#recording_hours'),'75');assert.equal(await field.inputValue(),'wav');
  fail=false;await page.locator('#recording_format_apply').click();await page.waitForFunction(()=>!recordingPending);
  assert.equal(fixture.status.recording.format,'wav');assert.equal(fixture.status.recording.duration_minutes,60);assert.equal(await page.inputValue('#recording_hours'),'75');
  await page.locator('#recording_duration_apply').click();await page.waitForFunction(()=>!recordingPending);assert.equal(fixture.status.recording.duration_minutes,75);
  await page.fill('#recording_segment','');await field.selectOption('flac');await page.locator('#recording_format_apply').click();await page.waitForFunction(()=>!recordingPending);
  assert.equal(fixture.status.recording.segment_minutes,60);assert.equal(await page.inputValue('#recording_segment'),'');assert.equal(fixture.status.recording.format,'flac');
  await page.locator('#recording_panel > .discard').click();await page.waitForFunction(()=>!sectionBusy.size);
  await page.selectOption('#recording_duration_mode','0');await page.evaluate(()=>loadStatus());assert.equal(await page.inputValue('#recording_hours'),'0');
  await page.locator('#recording_duration_apply').click();await page.waitForFunction(()=>!recordingPending);assert.equal(fixture.status.recording.duration_minutes,0);
  assert.match(await page.locator('#recording_capacity').textContent(),/^FLAC:/);
  const hint=await page.locator('#recording_settings').textContent();assert.match(hint,/15–50%/);assert(!hint.includes('300 MB'));assert(!hint.toLowerCase().includes('test'));
  assert.equal(await page.locator('#recording_settings table').count(),0);assert.equal(await page.locator('.settings-save-bar').count(),0);
  fs.mkdirSync('tmp/recording-flat-review',{recursive:true});await page.locator('#recording_panel').screenshot({path:`tmp/recording-flat-review/${packed?'embedded':'source'}-${width}-${theme}.png`});
  assert(!await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth));assert.deepEqual(errors,[]);await page.close();
 }}finally{await browser.close();}
 console.log('Recording format: source/embedded, 320/390/desktop, themes, typography, drafts, polling, revert, failure, save and discard passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
