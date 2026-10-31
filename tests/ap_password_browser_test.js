// Source and firmware assets, both screens, mobile/desktop and both themes.
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
function asset(name,symbol,packed){
 if(!packed)return fs.readFileSync('esp32-birdnet-mic/webui/'+name,'utf8');
 const h=fs.readFileSync('esp32-birdnet-mic/'+(name==='index.html'?'WebUI_gz.h':'SetupAssets_gz.h'),'utf8'),a=h.indexOf(symbol+'[]');
 return zlib.gunzipSync(Buffer.from([...h.slice(a,h.indexOf('};',a)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
}
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 let cases=0;const visualChecks=[];
 try{
  for(const packed of [false,true])for(const setup of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
   const page=await browser.newPage({viewport:{width,height:900},colorScheme:theme}),errors=[],writes=[];
   const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json','utf8'));
   const state={ok:true,complete:true,default_mode:'recording',connection:'home',connected:true,ssid:'Home test',ap_name:'BirdNETMic-TEST',ap_active:false,ap_ip:'192.168.4.1',ip:'192.168.1.31',test_state:'idle',chip_family:'ESP32-C5',board_name:'XIAO ESP32-C5',s3_board_supported:false,s3_board:0,antenna_supported:true,antenna:'external',battery_enabled:false};
   fixture.status.setup=state;fixture.status.recording={mode:'recording',supported:true,busy:false,cooling_pending:false,state:'saved',elapsed_seconds:0,written_kib:0,free_mib:1000,card_ready:true,dropped_samples:0,duration_minutes:0,segment_minutes:60,auto_start:false,logging:{sd_enabled:true,detailed:false,max_kib:1024,bytes:0,file:'/recording.log',error:''}};
   fixture.status.field_power={enabled:false,cutoff_mv:3500,confirm_seconds:30,hysteresis_mv:100,wifi_delay_seconds:3,sleep_after_save:true,state:'disabled',latched:false,start_allowed:true,can_acknowledge:false,sample_mv:4000,remaining_seconds:0};
   let saved='ABCD2345',failure=0,hold=null,release;
   page.on('pageerror',e=>{errors.push(e.message);console.error('UI error:',e.message);});
   await page.addInitScript(theme=>localStorage.setItem('theme',theme),theme);
   await page.route('http://ap.test/**',async route=>{
    const req=route.request(),path=new URL(req.url()).pathname;
    if(path==='/ap-password.js')return require('./ui_assets').apPassword(route,packed);
    if(path==='/')return route.fulfill({contentType:'text/html',body:asset(setup?'setup.html':'index.html',setup?'SETUP_HTML_GZ':'WEBUI_INDEX_GZ',packed)});
    if(path==='/setup.css'||path==='/setup.js')return route.fulfill({contentType:path.endsWith('css')?'text/css':'application/javascript',body:asset(path.slice(1),path.endsWith('css')?'SETUP_CSS_GZ':'SETUP_JS_GZ',packed)});
    if(path==='/api/setup/access'){
     assert.equal(req.method(),'POST');assert.equal(req.headers()['x-esp32mic-csrf'],'1');
     const body=new URLSearchParams(req.postData());
     if(body.has('password')){
      writes.push(Object.fromEntries(body));if(hold)await hold;
      if(failure===1)return route.fulfill({status:503,json:{ok:false,error:'Could not save the password. Try again.'}});
      if(failure===2)return route.fulfill({status:409,json:{ok:false,error:'Stop recording before changing the password.'}});
      if(failure===3)return route.abort();
      saved=body.get('password');
     }
     return route.fulfill({json:{ok:true,password:saved}});
    }
    if(path==='/api/setup')return route.fulfill({json:state});
    if(path==='/api/logs')return route.fulfill({body:''});
    if(req.method()==='POST')throw Error('Unexpected save outside AP password: '+path);
    return route.fulfill({json:fixture[path.slice(5)]||{ok:true}});
   });
   await page.goto('http://ap.test/');
   if(setup){await page.waitForFunction(()=>!document.getElementById('choices').disabled);await page.locator('#access_details').evaluate(e=>e.open=true);}
   else{await page.waitForFunction(()=>window.deviceApPassword&&deviceSetupState,{},{timeout:5000}).catch(async e=>{try{await page.evaluate(()=>loadStatus());}catch(cause){console.error(cause);}throw e;});await page.evaluate(()=>openSection('networksec'));}
   const input=page.locator('#ap_password_input'),dirty=()=>input.evaluate(e=>e.classList.contains('dirty'));
   await page.click('#ap_password_change');await input.waitFor({state:'visible'});
   assert.equal(await input.inputValue(),saved);assert(!await dirty());assert(await page.locator('#ap_password_save').isDisabled());
   await input.fill('Forest test & password!');assert(await dirty());assert.equal(await page.locator('#ap_password_dirty').textContent(),'Unsaved');
   const style=await input.evaluate((e,setup)=>{
    const field=getComputedStyle(e),reference=getComputedStyle(document.querySelector(setup?'#ssid':'#in_mdns_host'));
    const note=getComputedStyle(document.querySelector('#ap_password_dirty'));
    return {font:field.fontSize,family:field.fontFamily,referenceFont:reference.fontSize,referenceFamily:reference.fontFamily,
     border:field.borderTopColor,normalBorder:reference.borderTopColor,note:note.color,shadow:field.boxShadow};
   },setup);
   assert.equal(style.font,style.referenceFont);assert.equal(style.family,style.referenceFamily);
   assert.notEqual(style.border,style.normalBorder);assert.equal(style.border,style.note);assert.notEqual(style.shadow,'none');
   visualChecks.push({packed,setup,width,theme,...style});
   const typography=await page.evaluate(setup=>{
    const a=getComputedStyle(document.getElementById('ap_password_input')),b=getComputedStyle(document.getElementById(setup?'password':'in_mdns_host'));
    return {size:a.fontSize,other:b.fontSize,family:a.fontFamily,otherFamily:b.fontFamily,border:a.borderColor};
   },setup);
   assert.equal(typography.size,typography.other);assert.equal(typography.family,typography.otherFamily);assert.match(typography.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
   await page.evaluate(setup=>setup?poll():loadAll(true),setup);assert.equal(await input.inputValue(),'Forest test & password!');assert(await dirty());
   await input.fill(saved);assert(!await dirty());assert.equal(writes.length,0);
   assert.equal(await page.evaluate(()=>{const e=new Event('beforeunload',{cancelable:true});window.dispatchEvent(e);return e.defaultPrevented;}),false);
   await input.fill('short');await page.click('#ap_password_save');assert.equal(writes.length,0);assert(await dirty());assert.match(await page.locator('#ap_password_feedback').textContent(),/8–63/);
   await input.fill('Forest test & password!');
   for(const error of [1,2,3]){
    failure=error;await page.click('#ap_password_save');await page.waitForFunction(()=>!document.getElementById('ap_password_input').disabled);
    assert(await dirty());assert.equal(await input.inputValue(),'Forest test & password!');assert(await page.locator('#ap_password_feedback').textContent());
   }
   failure=0;hold=new Promise(r=>release=r);await page.click('#ap_password_save');await page.waitForFunction(()=>document.getElementById('ap_password_input').disabled);
   assert(await page.locator('#ap_password_cancel').isDisabled());release();hold=null;
   await page.locator('#ap_password_editor').waitFor({state:'hidden'});assert.equal(saved,'Forest test & password!');assert(!await dirty());
   assert(writes.every(w=>Object.keys(w).join()==='password'));assert.match(await page.locator('#ap_password_feedback').textContent(),/Password saved/);
   if(!setup){
    await page.fill('#in_mdns_host','keep-this-draft');
    await page.click('#ap_password_change');await input.waitFor({state:'visible'});await input.fill('Second password test');
    await page.click('#ap_password_save');await page.locator('#ap_password_editor').waitFor({state:'hidden'});
    assert.equal(await page.locator('#in_mdns_host').inputValue(),'keep-this-draft');assert(await page.locator('#in_mdns_host').evaluate(e=>e.classList.contains('dirty')));
    await page.locator('#networksec>.discard').click();
   }
   await page.click('#ap_password_change');await input.waitFor({state:'visible'});assert.equal(await input.inputValue(),saved);
   await page.click('#ap_password_show');assert.equal(await input.getAttribute('type'),'text');await page.click('#ap_password_show');
   await input.fill('Discard this password');assert(await dirty());
   assert.equal(await page.evaluate(()=>{const e=new Event('beforeunload',{cancelable:true});window.dispatchEvent(e);return e.defaultPrevented;}),true);
   if(setup)await page.click('#ap_password_cancel');else await page.locator('#networksec>.discard').click();
   assert(await page.locator('#ap_password_editor').isHidden());assert(!await dirty());
   await page.click('#ap_password_change');await input.waitFor({state:'visible'});assert.equal(await input.inputValue(),saved);
   await input.fill('Screenshot test draft');
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
   if(!packed)await page.screenshot({path:`tmp/ap-password-${setup?'setup':'network'}-${width}-${theme}.png`,fullPage:true});
   await page.click('#ap_password_cancel');assert.deepEqual(errors,[]);await page.close();cases++;
  }
  fs.writeFileSync('tmp/ap-password-visual-checks.json',JSON.stringify(visualChecks,null,2));
  console.log(JSON.stringify({ok:true,cases,typographyAndOrangeDraftsVerified:true}));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
