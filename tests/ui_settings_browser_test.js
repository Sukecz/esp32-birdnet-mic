const assert=require('node:assert/strict');
const fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 try{
 for(const packed of [false,true]){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json','utf8'));
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');source=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const page=await browser.newPage({viewport:{width:390,height:844}}),writes=[],errors=[];
  let failure='',hold=null,release=null,acceptDialog=false,thermalFailure=false;
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>acceptDialog?d.accept():d.dismiss());
  await page.route('http://ui.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(req.method()!=='GET'){
    const values=Object.fromEntries(new URLSearchParams(req.postData()));writes.push({path,values});
    if(hold){await hold;hold=null;}
    if(failure==='network')return route.abort();
    if(failure)return route.fulfill({json:{ok:false,error:failure}});
    if(path==='/api/set'&&values.key==='gain')fixture.audio_status.gain=Number(values.value);
    if(path==='/api/settings/schedule')Object.assign(fixture.status,{stream_schedule_enabled:values.enabled==='on',stream_schedule_start_min:Number(values.start),stream_schedule_stop_min:Number(values.stop),deep_sleep_sched_enabled:values.sleep==='on'});
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/thermal'&&thermalFailure)return route.abort();
   if(path==='/api/logs')return route.fulfill({body:'Example log'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.27'}});
  });
  await page.goto('http://ui.test/');await page.waitForFunction(()=>document.querySelector('#in_gain').value==='1.50');
  assert.match(await page.locator('#profile').textContent(),/Balanced/);
  assert.equal(await page.locator('#s1_stream').textContent(),'YES');
  await page.evaluate(()=>openSection('audiosec'));
  // Returning to the saved value clears markers, the discard button and unload warning.
  let initialWrites=writes.length;
  const dirty=async id=>page.locator('#'+id).evaluate(e=>e.classList.contains('dirty'));
  for(const value of ['2.75','', '1.51']){
   await page.locator('#in_gain').fill(value);assert(await dirty('in_gain'));
   await page.locator('#in_gain').fill('1.5');assert(!await dirty('in_gain')); // 1.50 == 1.5
  }
  assert(await page.locator('#audiosec>.discard').isHidden());
  await page.evaluate(()=>openSection('audioadvanced'));
  for(const [id,changed,original] of [['sel_buf','2048','512'],['sel_mic_format','1','0'],['sel_hp','off','on']]){
   await page.selectOption('#'+id,changed);assert(await dirty(id));
   await page.selectOption('#'+id,original);assert(!await dirty(id));
  }
  await page.evaluate(()=>openSection('networksec'));
  const host=await page.locator('#in_mdns_host').inputValue();
  await page.locator('#in_mdns_host').fill('changed-host');assert(await dirty('in_mdns_host'));
  await page.locator('#in_mdns_host').fill(' '+host+' ');assert(!await dirty('in_mdns_host'));
  await page.evaluate(()=>openSection('thermalsec'));
  const autoResume=await page.locator('#sel_oh_auto').inputValue();
  await page.selectOption('#sel_oh_auto',autoResume==='on'?'off':'on');assert(await dirty('sel_oh_auto'));
  await page.selectOption('#sel_oh_auto',autoResume);assert(!await dirty('sel_oh_auto'));
  await page.evaluate(()=>openSection('timesec'));
  const time=await page.locator('#in_stream_start').inputValue();
  await page.locator('#in_stream_start').fill('07:15');assert(await dirty('in_stream_start'));
  await page.locator('#in_stream_start').fill(time);assert(!await dirty('in_stream_start'));
  await page.evaluate(()=>openSection('mqttsec'));
  await page.locator('#in_mqtt_pass').fill(' draft ');assert(await dirty('in_mqtt_pass'));
  await page.locator('#in_mqtt_pass').fill('');assert(!await dirty('in_mqtt_pass'));
  await page.locator('#mqtt_clear_password').check();assert(await dirty('mqtt_clear_password'));
  await page.locator('#mqtt_clear_password').uncheck();assert(!await dirty('mqtt_clear_password'));
  assert.equal(writes.length,initialWrites);
  assert.equal(await page.evaluate(()=>Object.keys(edits).length),0);
  await page.evaluate(()=>refreshAll());assert.equal(await page.evaluate(()=>Object.keys(edits).length),0);
  await page.evaluate(()=>openSection('audiosec'));
  await page.locator('#in_gain').fill('2.75');
  await page.evaluate(async()=>{const now=Date.now;Date.now=()=>now()+120000;try{await refreshAll();}finally{Date.now=now;}});
  assert.equal(await page.locator('#in_gain').inputValue(),'2.75');
  assert.match(await page.locator('#in_gain').locator('..').innerText(),/Unsaved/);
  failure='invalid_value';await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent.includes('rejected'));
  assert.equal(await page.locator('#in_gain').inputValue(),'2.75');
  assert(await page.locator('#in_gain').evaluate(e=>e.classList.contains('dirty')));
  failure='network';await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent.includes('Could not confirm'));
  await page.evaluate(()=>refreshAll());assert.equal(await page.locator('#in_gain').inputValue(),'2.75');
  failure='';await page.locator('#btn_gain_set').click();await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saved.');
  await page.waitForFunction(()=>!document.querySelector('#in_gain').classList.contains('dirty'));
  assert.equal(fixture.audio_status.gain,2.75);
  // A newer edit survives the successful response to an older save.
  hold=new Promise(r=>release=r);await page.locator('#in_gain').fill('3');await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saving…');
  await page.locator('#in_gain').fill('4');release();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saved.');
  assert.equal(await page.locator('#in_gain').inputValue(),'4');
  await page.waitForFunction(()=>!document.querySelector('#audiosec>.discard').disabled);
  await page.locator('#audiosec>.discard').click();await page.waitForFunction(()=>document.querySelector('#in_gain').value==='3.00');
  // A revert while a save is pending stays intact when the saved baseline changes.
  hold=new Promise(r=>release=r);await page.locator('#in_gain').fill('5');await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saving…');
  await page.locator('#in_gain').fill('3');assert(!await dirty('in_gain'));release();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saved.'&&!sectionBusy.has('audiosec'));
  assert.equal(await page.locator('#in_gain').inputValue(),'3');assert(await dirty('in_gain'));
  await page.locator('#in_gain').fill('5');assert(!await dirty('in_gain'));
  // A newer edit equal to the successfully saved value becomes clean after confirmation.
  hold=new Promise(r=>release=r);await page.locator('#in_gain').fill('7');await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback').textContent==='Saving…');
  await page.locator('#in_gain').fill('8');await page.locator('#in_gain').fill('7');release();
  await page.waitForFunction(()=>!sectionBusy.has('audiosec')&&document.querySelector('#audiosec>.feedback').textContent==='Saved.');
  assert.equal(await page.locator('#in_gain').inputValue(),'7');assert(!await dirty('in_gain'));
  // The next poll also removes a failed-save draft when it equals the confirmed state.
  failure='network';await page.locator('#in_gain').fill('9');await page.locator('#btn_gain_set').click();
  await page.waitForFunction(()=>!sectionBusy.has('audiosec')&&document.querySelector('#audiosec>.feedback').textContent.includes('Could not confirm'));
  await page.locator('#in_gain').fill('7');assert(!await dirty('in_gain'));
  await page.evaluate(()=>refreshAll());assert(!await dirty('in_gain'));
  assert.equal(await page.evaluate(()=>Object.keys(edits).length),0);failure='';
  assert.equal(await page.locator('#sel_cpu option[value="120"]').count(),0);
  // One request saves the whole cross-midnight window.
  await page.evaluate(()=>openSection('timesec'));await page.selectOption('#sel_stream_sched_en','on');
  await page.locator('#in_stream_start').fill('22:00');await page.locator('#in_stream_stop').fill('22:00');
  let count=writes.length;await page.locator('#btn_schedule_save').click();assert.equal(writes.length,count);
  await page.locator('#in_stream_stop').fill('06:00');await page.locator('#btn_schedule_save').click();
  await page.waitForFunction(()=>document.querySelector('#timesec>.feedback').textContent==='Schedule saved.');
  assert.equal(writes.length,count+1);assert.deepEqual(writes.at(-1),{path:'/api/settings/schedule',values:{enabled:'on',start:'1320',stop:'360',sleep:'off'}});
  // A cancelled sleep confirmation sends nothing.
  await page.selectOption('#sel_deep_sleep_sched','on');count=writes.length;await page.locator('#btn_schedule_save').click();assert.equal(writes.length,count);
  // Password changes and removal are explicit; all broker fields share one request.
  await page.evaluate(()=>openSection('mqttsec'));await page.locator('#in_mqtt_host').fill('new.example');
  count=writes.length;await page.locator('#btn_mqtt_save').click();await page.waitForFunction(()=>document.querySelector('#mqttsec>.feedback').textContent.includes('settings saved'));
  assert.equal(writes.length,count+1);assert.equal(writes.at(-1).values.password,'');assert.equal(writes.at(-1).values.clear_password,'off');
  await page.waitForFunction(()=>!document.querySelector('#mqttsec>.discard').disabled);
  await page.locator('#in_mqtt_pass').fill(' example password ');failure='network';await page.locator('#btn_mqtt_save').click();
  await page.waitForFunction(()=>document.querySelector('#mqttsec>.feedback').textContent.includes('Could not confirm'));
  assert.equal(await page.locator('#in_mqtt_pass').inputValue(),' example password ');
  failure='';await page.locator('#btn_mqtt_save').click();await page.waitForFunction(()=>document.querySelector('#in_mqtt_pass').value==='');
  assert.equal(writes.at(-1).values.password,' example password ');
  await page.locator('#mqtt_clear_password').check();await page.locator('#btn_mqtt_save').click();await page.waitForFunction(()=>!document.querySelector('#mqtt_clear_password').checked);
  assert.equal(writes.at(-1).values.clear_password,'on');
  // Accessible help and intentional pauses.
  assert.equal(await page.locator('.help').evaluateAll(es=>es.filter(e=>e.tabIndex<0).length),0);
  await page.locator('#h_gain').focus();await page.keyboard.press('Enter');assert.equal(await page.locator('#h_gain').getAttribute('aria-expanded'),'true');
  fixture.status.s1_streaming=false;fixture.status.s2_streaming=false;fixture.status.stream_schedule_enabled=false;
  await page.evaluate(()=>refreshAll());assert.match(await page.locator('#overview_streams').innerText(),/waiting for a client/);
  fixture.status.stream_schedule_enabled=true;fixture.status.stream_schedule_allow_now=false;
  await page.evaluate(()=>refreshAll());assert.match(await page.locator('#overview_streams').innerText(),/schedule until 06:00|schedule until 22:00/);
  fixture.thermal.latched_persist=true;fixture.thermal.auto_resume_pending=true;
  await page.evaluate(()=>refreshAll());assert.match(await page.locator('#overview_streams').innerText(),/cooling/);
  thermalFailure=true;await page.evaluate(()=>refreshAll());assert.match(await page.locator('#connection_status').innerText(),/out of date/);
  await page.evaluate(()=>loadAll(false));assert.match(await page.locator('#connection_status').innerText(),/out of date/);
  thermalFailure=false;await page.evaluate(()=>refreshAll());assert.equal(await page.locator('#connection_status').innerText(),'');
  await page.evaluate(()=>openSection('recoverysec'));count=writes.length;await page.locator('#b_defaults').click();assert.equal(writes.length,count);
  acceptDialog=true;await page.locator('#b_defaults').click();await page.waitForFunction(()=>document.querySelector('#ovr').style.display==='flex');
  assert.equal(writes.at(-1).path,'/api/action/factory_reset');
  assert.deepEqual(errors,[]);await page.close();console.log((packed?'Embedded':'Source')+' UI: returned/equivalent values, pending-save reverts, confirmed baselines, persistent drafts, rejected/offline/concurrent saves, discard, grouped schedule/MQTT, password semantics, reset confirmation, state reasons and keyboard help passed');
 }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
