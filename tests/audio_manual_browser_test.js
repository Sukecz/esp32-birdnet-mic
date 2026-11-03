const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
const assets=require('./ui_assets');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 fs.mkdirSync('tmp/qualification/feature-groups',{recursive:true});fs.mkdirSync('tmp/qualification/manual-ui',{recursive:true});fs.mkdirSync('tmp/qualification/2.06-manual-ui',{recursive:true});
 const results=[];
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.audio_status.s3_board_supported=true;fixture.audio_status.s3_board=0;
  fixture.audio_status.noise={mode:0,floor_db:12,adapt_s:15,active:false,learn_pct:0,cpu_pct:0,cpu_peak_pct:0,est_reduction_db:0};
  const writes=[],errors=[];let fail=false,graphReads=0;
  const page=await browser.newPage({viewport:{width,height:1000}});page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://auto.test/**',route=>{
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:assets.main(packed)});
   if(path==='/ap-password.js')return assets.apPassword(route,packed);
   if(route.request().method()==='POST'){
    const v=Object.fromEntries(new URLSearchParams(route.request().postData()));writes.push({path,v});
    if(fail)return route.fulfill({status:500,json:{ok:false,error:'test failure'}});
    if(path==='/api/noise/set')Object.assign(fixture.audio_status.noise,{mode:Number(v.mode),floor_db:Number(v.floor),adapt_s:Number(v.adapt)});
    if(path==='/api/settings/s3_board')fixture.audio_status.s3_board=Number(v.variant);
    if(path==='/api/settings/microphone')fixture.audio_status.mic_format=Number(v.format);
    if(path==='/api/set'&&v.key==='hp_cutoff')fixture.audio_status.hp_cutoff_hz=Number(v.value);
    if(path==='/api/set'&&v.key==='hp_enable')fixture.audio_status.hp_enable=v.value==='on';
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/microphone_profile')return route.fulfill({json:{ok:true,mic_format:Number(new URL(route.request().url()).searchParams.get('format')),sample_rate:48000,gain:1.5,buffer_size:512,i2s_shift:12,hp_enable:true,hp_cutoff_hz:500,cpu_mhz:0,board:'XIAO ESP32-S3 Sense'}});
   if(path==='/api/noise'){graphReads++;return route.fulfill({json:fixture.audio_status.noise});}
   if(path==='/api/logs')return route.fulfill({body:'test'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
  });
  await page.goto('http://auto.test/');await page.waitForFunction(()=>document.getElementById('in_gain').value==='1.50');
  await page.selectOption('#themeSel',theme);await page.evaluate(()=>{openSection('audiosec');openSection('noisesec');});
  for(const [control,button] of [['noise_mode','noise_save'],['noise_floor','noise_floor_apply'],['noise_adapt','noise_adapt_apply']]){
   assert.equal(await page.locator('#'+button).textContent(),'Apply');
   const style=await page.locator('#'+button).evaluate((e,id)=>{const ref=document.getElementById('btn_hp_set');return {sameField:e.closest('.field')===document.getElementById(id).closest('.field'),font:getComputedStyle(e).fontSize,reference:getComputedStyle(ref).fontSize,padding:getComputedStyle(e).padding,paddingReference:getComputedStyle(ref).padding};},control);
   assert(style.sameField);assert.equal(style.font,style.reference);assert.equal(style.padding,style.paddingReference);
   assert(await page.locator('#'+control).isVisible());assert(await page.locator('#'+button).isDisabled());
  }
  assert.equal(await page.locator('#noisesec details').count(),0);
  assert(await page.locator('#noise_spectrum').isHidden());assert(await page.locator('#noise_graph_placeholder').isVisible());assert(await page.locator('#noise_status').isHidden());
  const text=await page.locator('#noisesec').textContent();assert(!/C6|160 MHz|80 MHz|Save noise reduction/.test(text));assert.match(text,/Low CPU speeds.*dropouts/);
  assert.equal(await page.locator('#audioadvanced').count(),0);
  for(const id of ['in_rate','sel_buf','in_shift'])assert(await page.locator('#'+id).isVisible());
  for(const id of ['sel_hp','in_hp_cutoff','noise_mode'])assert.equal(await page.locator('#'+id).evaluate(e=>e.closest('details.card').id),'noisesec');
  for(const id of ['sel_s3_board','sel_mic_format'])assert.deepEqual(await page.locator('#'+id+' option').evaluateAll(es=>es.map(e=>e.value)),['0','1','2']);
  assert(await page.locator('#noise_busy_hint').isHidden());
  assert.match(await page.locator('#noise_help').textContent(),/High-pass cuts low rumble/);
  const dirty=async id=>page.locator('#'+id).evaluate(e=>e.classList.contains('dirty'));
  for(const [id,changed,original] of [['sel_s3_board','1','0'],['sel_mic_format','1','0'],['noise_mode','1','0']]){
   await page.selectOption('#'+id,changed);assert(await dirty(id));
   await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#'+id),changed);
   await page.selectOption('#'+id,original);assert(!await dirty(id));
  }
  assert.equal(writes.length,0);
  assert(await page.locator('#noise_floor').isDisabled());assert(await page.locator('#noise_adapt').isDisabled());
  await page.selectOption('#noise_mode','1');assert(await page.locator('#noise_floor').isDisabled(),'On must be applied first');
  await page.fill('#in_gain','2.25');await page.fill('#in_hp_cutoff','1500');fail=true;await page.click('#noise_save');
  await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(await dirty('noise_mode'));assert(await page.locator('#noise_floor').isDisabled());
  assert.match(await page.locator('#noisesec>.feedback').textContent(),/kept/);
  fail=false;await page.click('#noise_save');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
  assert(!await dirty('noise_mode'));assert(await page.locator('#noise_floor').isEnabled());assert(await dirty('in_gain'));
  assert.deepEqual(writes.at(-1).v,{mode:'1',floor:'12',adapt:'15'});
  await page.waitForFunction(()=>!noiseGraphBusy);const reads=graphReads;await page.evaluate(()=>updateNoiseSpectrum());assert(graphReads>reads);
  const openReads=graphReads;await page.evaluate(()=>{document.getElementById('noisesec').open=false;return updateNoiseSpectrum();});assert.equal(graphReads,openReads);await page.evaluate(()=>openSection('noisesec'));
  for(const [id,changed,original] of [['noise_floor','6','12'],['noise_adapt','60','15']]){
   await page.selectOption('#'+id,changed);assert(await dirty(id));await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#'+id),changed);
   await page.selectOption('#'+id,original);assert(!await dirty(id));
  }
  await page.selectOption('#noise_floor','6');await page.selectOption('#noise_adapt','60');
  await page.selectOption('#noise_mode','0');assert(await page.locator('#noise_floor').isDisabled());await page.click('#noise_save');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
  assert(await dirty('noise_floor'));assert(await dirty('noise_adapt'));assert.deepEqual(writes.at(-1).v,{mode:'0',floor:'12',adapt:'15'});
  await page.selectOption('#noise_mode','1');await page.click('#noise_save');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
  assert(await dirty('noise_floor'));assert(await dirty('noise_adapt'));assert.deepEqual(writes.at(-1).v,{mode:'1',floor:'12',adapt:'15'});
  await page.click('#noise_floor_apply');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(!await dirty('noise_floor'));assert(await dirty('noise_adapt'));
  assert.deepEqual(writes.at(-1).v,{mode:'1',floor:'6',adapt:'15'});
  await page.selectOption('#noise_mode','0');assert(await page.locator('#noise_adapt_apply').isDisabled());
  const blocked=writes.length;await page.evaluate(()=>saveNoiseSettings('noise_adapt'));assert.equal(writes.length,blocked);
  await page.selectOption('#noise_mode','1');await page.click('#noise_adapt_apply');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(!await dirty('noise_adapt'));
  assert.deepEqual(writes.at(-1).v,{mode:'1',floor:'6',adapt:'60'});
  await page.selectOption('#noise_adapt','15');await page.click('#noise_adapt_apply');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
  assert.deepEqual(Object.keys(writes.at(-1).v).sort(),['adapt','floor','mode']);
  assert(await dirty('in_hp_cutoff'),'Noise save preserves a High-pass draft');
  assert.equal(await page.inputValue('#sel_hp'),'on','Noise save preserves High-pass');
  assert(fixture.audio_status.hp_enable,'High-pass remains enabled on the device fixture');
  await page.selectOption('#noise_adapt','60');fail=true;await page.click('#btn_hpf_cut_set');
  await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(await dirty('in_hp_cutoff'));assert(await dirty('noise_adapt'));
  fail=false;await page.click('#btn_hpf_cut_set');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
  assert(!await dirty('in_hp_cutoff'));assert(await dirty('noise_adapt'));assert(await dirty('in_gain'));
  assert.deepEqual(writes.at(-1).v,{key:'hp_cutoff',value:'1500'});
  await page.selectOption('#sel_hp','off');await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#sel_hp'),'off');assert(await dirty('sel_hp'));
  await page.click('#btn_hp_set');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(!await dirty('sel_hp'));assert(await dirty('noise_adapt'));assert(await page.locator('#in_hp_cutoff').isDisabled());
  await page.selectOption('#sel_hp','on');await page.click('#btn_hp_set');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));assert(!await dirty('sel_hp'));assert(fixture.audio_status.hp_enable);assert(await dirty('noise_adapt'));

  await page.locator('#audiosec>.discard').click();assert(!await dirty('in_gain'));assert(await dirty('noise_adapt'),'Discard Audio preserves filter drafts');
  await page.locator('#noisesec>.discard').click();assert(!await dirty('noise_adapt'));

  await page.selectOption('#noise_adapt','60');assert(await page.locator('#noise_adapt_apply').isEnabled());
  fixture.status.recording={...(fixture.status.recording||{}),busy:true};await page.evaluate(()=>refreshAll());
  for(const id of ['noise_save','noise_floor_apply','noise_adapt_apply'])assert(await page.locator('#'+id).isDisabled());assert(await page.locator('#noise_busy_hint').isVisible());
  fixture.status.recording.busy=false;await page.evaluate(()=>refreshAll());assert(await page.locator('#noise_busy_hint').isHidden());assert(await page.locator('#noise_adapt_apply').isEnabled());await page.selectOption('#noise_adapt','15');
  fixture.audio_status.noise.error='CPU too slow; audio is unfiltered. Raise CPU frequency.';
  await page.evaluate(()=>refreshAll());assert.match(await page.locator('#noise_status').textContent(),/Cannot keep up.*Audio is unchanged/);
  fixture.audio_status.noise.error='';await page.evaluate(()=>refreshAll());
  await page.selectOption('#noise_adapt','60');await page.locator('#noisesec>.discard').click();
  assert.equal(await page.inputValue('#noise_adapt'),'15');assert(!await dirty('noise_adapt'));
  // Saved board and mic choices are independent, and saving one preserves other drafts.
  await page.selectOption('#sel_mic_format','1');await page.selectOption('#sel_s3_board','1');
  await page.click('#btn_s3_board_apply');await page.waitForFunction(()=>!sectionBusy.has('audiosec'));
  assert(await dirty('sel_mic_format'));assert(!await dirty('sel_s3_board'));
  await page.click('#btn_mic_apply');await page.locator('#mic_dialog').waitFor();await page.click('#btn_mic_keep');await page.waitForFunction(()=>!sectionBusy.has('audiosec'));
  assert(!await dirty('sel_mic_format'));
  let count=writes.length;await page.evaluate(()=>{saveS3Board();chooseMicSettings();saveNoiseSettings();saveNoiseSettings('noise_floor');saveNoiseSettings('noise_adapt');});assert.equal(writes.length,count);
  await page.selectOption('#noise_adapt','60');
  const styles=await page.locator('#noise_adapt').evaluate(e=>({border:getComputedStyle(e).borderColor,font:getComputedStyle(e).fontSize,reference:getComputedStyle(document.getElementById('sel_hp')).fontSize}));
  assert.equal(styles.font,styles.reference);assert.match(styles.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  if(packed){await page.locator('#audiosec').screenshot({path:`tmp/qualification/feature-groups/audio-${width}-${theme}.png`});await page.locator('#noisesec').screenshot({path:`tmp/qualification/feature-groups/filters-${width}-${theme}.png`});}
  await page.evaluate(()=>openSection('thermalsec'));
  assert.equal(await page.locator('#t_thermal').textContent(),'Thermal & performance');
  assert.deepEqual(await page.locator('#thermalsec>h3').allTextContents(),['CPU performance','Temperature protection']);
  const thermal=await page.locator('#sel_cpu').evaluate(e=>({separate:e.closest('table')!==document.getElementById('sel_oh_enable').closest('table'),font:getComputedStyle(e).fontSize,reference:getComputedStyle(document.getElementById('sel_oh_enable')).fontSize,heading:getComputedStyle(document.querySelector('#thermalsec>h3')).fontSize,headingReference:getComputedStyle(document.querySelector('#networksec h3')).fontSize}));
  assert(thermal.separate);assert.equal(thermal.font,thermal.reference);assert.equal(thermal.heading,thermal.headingReference);
  if(packed)await page.locator('#thermalsec').screenshot({path:`tmp/qualification/manual-ui/performance-${width}-${theme}.png`});
  assert.deepEqual(errors,[]);results.push({packed,width,theme,styles,thermal});await page.close();
 }
 fs.writeFileSync('tmp/qualification/manual-ui/results.json',JSON.stringify(results,null,2));
 console.log('Manual board/mic + noise: source/embedded, 320/390/1280, light/dark, typography, drafts, polling, revert/save/error/discard and scopes passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
