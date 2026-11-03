const assert=require('node:assert/strict'),fs=require('node:fs'),assets=require('./ui_assets');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 fs.mkdirSync('tmp/qualification/feature-groups',{recursive:true});const results=[];
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.audio_status.hp_enable=false;fixture.audio_status.noise={mode:0,floor_db:12,adapt_s:15,active:false,learned:false,learn_pct:0};
  fixture.thermal.protection_enabled=false;fixture.status.mqtt_enabled=false;fixture.status.mdns_enabled=false;
  const writes=[],errors=[];let fail=false;
  const page=await browser.newPage({viewport:{width,height:1000}});page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://features.test/**',route=>{
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:assets.main(packed)});
   if(path==='/ap-password.js')return assets.apPassword(route,packed);
   if(route.request().method()!=='GET'){
    if(path==='/api/setup/access')return route.fulfill({status:403,json:{ok:false,error:'Credential read omitted'}});
    const v=Object.fromEntries(new URLSearchParams(route.request().postData()));writes.push({path,v});
    if(fail)return route.fulfill({status:500,json:{ok:false,error:'test_failure'}});
    if(path==='/api/noise/set')Object.assign(fixture.audio_status.noise,{mode:Number(v.mode),floor_db:Number(v.floor),adapt_s:Number(v.adapt)});
    const fields={hp_enable:['audio_status','hp_enable',x=>x==='on'],hp_cutoff:['audio_status','hp_cutoff_hz',Number],sched_reset:['perf_status','scheduled_reset',x=>x==='on'],reset_hours:['perf_status','reset_hours',Number],oh_enable:['thermal','protection_enabled',x=>x==='on'],oh_limit:['thermal','shutdown_c',Number],oh_auto:['thermal','auto_resume_enabled',x=>x==='on']};
    if(path==='/api/set'&&fields[v.key]){const [root,key,convert]=fields[v.key];fixture[root][key]=convert(v.value);}
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:'test'});
   if(path==='/api/noise')return route.fulfill({json:fixture.audio_status.noise});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
  });
  await page.goto('http://features.test/');await page.waitForFunction(()=>['noise_mode','sel_hp','in_sched','sel_oh_enable'].every(id=>document.getElementById(id).dataset.savedLoaded==='1'));
  await page.selectOption('#themeSel',theme);await page.evaluate(()=>{for(const id of ['noisesec','recoverysec','thermalsec','timesec','networksec','mqttsec','batterysec'])openSection(id);});
  const groups=[['noise_mode','1','0','noise_floor','6','noise_save','noise_floor_apply','noisesec','noise_options'],['sel_hp','on','off','in_hp_cutoff','1500','btn_hp_set','btn_hpf_cut_set','noisesec','hpf_options'],['in_sched','on','off','in_hours','12','btn_sched_set','btn_hours_set','recoverysec','reboot_options'],['sel_oh_enable','on','off','sel_oh_limit','80','btn_oh_enable','btn_oh_limit','thermalsec','thermal_options']];
  const dirty=async id=>page.locator('#'+id).evaluate(e=>e.classList.contains('dirty'));
  const change=async(id,value)=>{if(await page.locator('#'+id).evaluate(e=>e.tagName==='SELECT'))await page.selectOption('#'+id,value);else await page.fill('#'+id,value);};
  for(const [parent,on,off,child,value,apply,childApply,section,group] of groups){
   assert.equal(await page.inputValue('#'+parent),off);assert(await page.locator('#'+child).isDisabled());
   assert(await page.locator('#'+group).evaluate(e=>e.classList.contains('is-disabled')));
   const layout=await page.locator('#'+group).evaluate(e=>{const main=e.previousElementSibling,p=e.getBoundingClientRect(),r=main.getBoundingClientRect();return {indent:p.left-r.left,line:getComputedStyle(e).borderLeftWidth,inside:p.right<=e.closest('details.card').getBoundingClientRect().right};});
   assert(layout.indent>=8);assert.equal(layout.line,'2px');assert(layout.inside);
   await page.selectOption('#'+parent,on);assert(await dirty(parent));await page.evaluate(()=>refreshAll());
   assert.equal(await page.inputValue('#'+parent),on);assert(await page.locator('#'+child).isDisabled(),'An unsaved On selection cannot enable details');
   if(parent==='noise_mode'){const n=writes.length;await page.evaluate(()=>saveNoiseSettings('noise_floor'));assert.equal(writes.length,n);}
   fail=true;await page.click('#'+apply);await page.waitForFunction(id=>!sectionBusy.has(id),section);assert(await dirty(parent));assert(await page.locator('#'+child).isDisabled());
   fail=false;await page.click('#'+apply);await page.waitForFunction(id=>!sectionBusy.has(id),section);assert(!await dirty(parent));assert(await page.locator('#'+child).isEnabled());
   await change(child,value);assert(await dirty(child));await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#'+child),value);
   await page.selectOption('#'+parent,off);assert(await page.locator('#'+child).isDisabled());assert(await dirty(child));
   await page.click('#'+apply);await page.waitForFunction(id=>!sectionBusy.has(id),section);assert(await page.locator('#'+child).isDisabled());assert(await dirty(child),'Off preserves dependent drafts');
   const border=await page.locator('#'+child).evaluate(e=>getComputedStyle(e).borderColor);assert.match(border,/rgb\((217, 119, 6|245, 158, 11)\)/);
   await page.selectOption('#'+parent,on);assert(await page.locator('#'+child).isDisabled());await page.click('#'+apply);await page.waitForFunction(id=>!sectionBusy.has(id),section);
   assert.equal(await page.inputValue('#'+child),value);assert(await page.locator('#'+child).isEnabled());
   fail=true;await page.click('#'+childApply);await page.waitForFunction(id=>!sectionBusy.has(id),section);assert(await dirty(child));
   fail=false;await page.click('#'+childApply);await page.waitForFunction(id=>!sectionBusy.has(id),section);assert(!await dirty(child));
   const count=writes.length;await change(child,child==='noise_floor'?'12':child==='sel_oh_limit'?'85':'24');await page.locator('#'+section+'>.discard').click();assert.equal(writes.length,count);assert.equal(await page.inputValue('#'+child),value);
  }
  // Grouped configuration remains editable while Off so it can be prepared safely.
  for(const [id,group] of [['in_mqtt_host','mqtt_options'],['in_mdns_host','mdns_options'],['in_stream_start','schedule_options']]){
   assert(await page.locator('#'+id).isEnabled());assert.equal(await page.locator('#'+id).evaluate(e=>e.closest('.feature-settings').id),group);
  }
  for(const id of ['battery_options','battery_guard_options','sd_log_options'])assert.equal(await page.locator('#'+id).count(),1);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  const labels=await page.locator('#hpf_options').evaluate(e=>({child:getComputedStyle(e.querySelector('td.k')).fontSize,parent:getComputedStyle(e.previousElementSibling.querySelector('td.k')).fontSize}));assert.equal(labels.child,labels.parent);
  if(packed){
   await page.locator('#noisesec').screenshot({path:`tmp/qualification/feature-groups/on-${width}-${theme}.png`});
   await page.selectOption('#noise_floor','12');await page.selectOption('#noise_mode','0');await page.click('#noise_save');await page.waitForFunction(()=>!sectionBusy.has('noisesec'));
   assert(await dirty('noise_floor'));assert(await page.locator('#noise_floor').isDisabled());assert(await page.locator('#noise_graph_placeholder').isVisible());assert(await page.locator('#noise_spectrum').isHidden());
   await page.locator('#noisesec').screenshot({path:`tmp/qualification/feature-groups/off-draft-${width}-${theme}.png`});
   await page.locator('#noisesec>.discard').click();assert(!await dirty('noise_floor'));assert.equal(await page.inputValue('#noise_floor'),'6');
  }
  assert.deepEqual(errors,[]);results.push({packed,width,theme,labels});await page.close();
 }
 fs.writeFileSync('tmp/qualification/feature-groups/browser-results.json',JSON.stringify(results,null,2));
 console.log('Shared feature hierarchy: source/embedded, 320/390/1280 light/dark, indentation/typography/overflow, confirmed enable, parent/child failures, disabled orange drafts, scoped save/discard, grouped-form preparation and graph placeholder passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
