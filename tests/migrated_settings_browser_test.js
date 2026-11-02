const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 const errors=[];let cases=0;
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.status.recording={mode:'recording',state:'ready',supported:true,busy:false,card_ready:true,cooling_pending:false,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:1000,dropped_samples:0,file:'',error:'',logging:{sd_enabled:true,detailed:false,max_kib:977,file:'system.log',bytes:0,error:''}};
  fixture.status.field_power={enabled:false,cutoff_mv:3500,confirm_seconds:30,hysteresis_mv:100,wifi_delay_seconds:3,sleep_after_save:true,state:'disabled',latched:false,start_allowed:true};
  fixture.status.battery={supported:true,enabled:true,valid:true,voltage:4,percent:80,calibration_percent:0};
  let html=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');html=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const page=await browser.newPage({viewport:{width,height:900}}),posts=[];let failure=false;
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://groups.test/**',route=>{
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:html});
   if(path==='/ap-password.js')return require('./ui_assets').apPassword(route,packed);
   if(req.method()==='POST'){
    const v=Object.fromEntries(new URLSearchParams(req.postData()));posts.push({path,v});
    if(failure)return route.abort();
    if(path==='/api/recording/settings')fixture.status.recording.auto_start=v.auto_start==='1';
    if(path==='/api/recording/log/settings')fixture.status.recording.logging.detailed=v.detailed==='1';
    if(path==='/api/battery')fixture.status.battery.enabled=v.enabled==='1';
    if(path==='/api/field_power')for(const [k,n] of Object.entries(v))fixture.status.field_power[k]=Number(n);
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:''});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
  });
  await page.goto('http://groups.test/');await page.waitForFunction(()=>fieldPowerState&&batteryState&&recordingLogState);
  // Feedback and dirty styles change before asynchronous save/discard handlers
  // finish refreshing. Start the next scenario only after that transaction ends.
  const settled=()=>page.waitForFunction(()=>!pollBusy&&!recordingPending&&!recordingLogPending&&!batteryPending&&!fieldPowerPending&&!sectionBusy.size);
  await settled();await page.selectOption('#themeSel',theme);
  const groups=[['recording_auto','recording_save','recording_panel','recording_feedback',false,true],['recording_log_level','recording_log_save','logssec','recording_log_feedback','0','1'],['battery_monitor','battery_save','batterysec','battery_feedback',true,false],['field_wifi_delay','field_wifi_save','recording_panel','field_wifi_feedback','3','5'],['field_cutoff','field_power_save','batterysec','field_power_feedback','3.50','3.60']];
  for(const [id,save,root,feedback,original,changed] of groups){
   if(root!=='recording_panel')await page.evaluate(id=>openSection(id),root);
   if(root==='logssec')await page.evaluate(()=>document.getElementById('recording_log_settings').open=true);
   const input=page.locator('#'+id),change=async value=>{if(typeof value==='boolean')await input.setChecked(value);else if(id==='recording_log_level')await input.selectOption(value);else await input.fill(value);};
   const before=posts.length;await change(changed);assert(await input.evaluate(e=>e.classList.contains('dirty')));
   const style=await input.evaluate(e=>{const target=e.type==='checkbox'?e.closest('label'):e;return {color:getComputedStyle(target).borderColor,size:getComputedStyle(target).fontSize};});
   assert.match(style.color,/rgb\((217, 119, 6|245, 158, 11)\)/);assert(parseFloat(style.size)>=12);
   await page.evaluate(()=>refreshAll());assert(await input.evaluate(e=>e.classList.contains('dirty')));
   await change(original);assert(!await input.evaluate(e=>e.classList.contains('dirty')));assert.equal(posts.length,before);
   await change(changed);failure=true;await page.locator('#'+save).click();
   await page.waitForFunction(id=>/kept|fetch|connection|confirm/i.test(document.getElementById(id).textContent),feedback);
   assert(await input.evaluate(e=>e.classList.contains('dirty')));await settled();failure=false;
   await page.locator('#'+root+'>[data-discard-drafts]').click();await page.waitForFunction(id=>!document.getElementById(id).classList.contains('dirty'),id);await settled();
   await change(changed);await page.locator('#'+save).click();await page.waitForFunction(id=>!document.getElementById(id).classList.contains('dirty'),id).catch(async error=>{console.error({packed,width,theme,id,feedback:await page.locator('#'+feedback).textContent(),posts:posts.slice(-3),inputs:await page.evaluate(()=>fieldPowerIds.map(id=>{const e=document.getElementById(id);return {id,value:e.value,valid:e.checkValidity(),message:e.validationMessage};}))});throw error;});
   await settled();
   if(id==='field_wifi_delay')assert.deepEqual(Object.keys(posts.at(-1).v),['wifi_delay_seconds']);
   if(id==='field_cutoff')assert(!Object.hasOwn(posts.at(-1).v,'wifi_delay_seconds'));
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));cases++;
  }
  await page.close();
 }
 assert.deepEqual(errors,[]);console.log(`Migrated groups: ${cases} source/embedded/viewport/theme cases; orange drafts, polling, revert, failure/discard, scoped save and overflow passed`);
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
