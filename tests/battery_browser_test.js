// NODE_PATH=tmp/browser-tools/node_modules node tests/battery_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
function asset(file,symbol,packed){
 if(!packed)return fs.readFileSync('esp32-birdnet-mic/webui/'+file,'utf8');
 const h=fs.readFileSync('esp32-birdnet-mic/'+(file==='index.html'?'WebUI_gz.h':'SetupAssets_gz.h'),'utf8');
 const start=h.indexOf(symbol+'[]');assert(start>=0);
 return zlib.gunzipSync(Buffer.from([...h.slice(start,h.indexOf('};',start)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
}
const base={ok:true,complete:true,default_mode:'streaming',connection:'home',connected:true,ssid:'Home',ap_name:'BirdNETMic-TEST',ap_active:false,ip:'192.168.1.123',test_state:'idle',test_error:'',antenna:'external',s3_board:0,battery_enabled:false,battery_supported:true};
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const errors=[];fs.mkdirSync('tmp/battery-monitoring',{recursive:true});
 try{
  for(const packed of [false,true])for(const family of ['C3','S3','C5','C6']){
   const page=await browser.newPage({viewport:{width:320,height:850}});
   page.on('pageerror',e=>errors.push(e.message));
   let saved,info={...base,chip_family:'ESP32-'+family,board_name:'XIAO ESP32-'+family,s3_board_supported:family==='S3',antenna_supported:family==='C6'};
   await page.route('http://battery.test/**',route=>{
    const path=new URL(route.request().url()).pathname;
    if(path==='/')return route.fulfill({contentType:'text/html',body:asset('setup.html','SETUP_HTML_GZ',packed)});
    if(path==='/setup.js'||path==='/setup.css')return route.fulfill({contentType:path.endsWith('js')?'application/javascript':'text/css',body:asset(path.slice(1),path.endsWith('js')?'SETUP_JS_GZ':'SETUP_CSS_GZ',packed)});
    if(path==='/api/setup/access')return route.fulfill({json:{ok:true,password:'TEST2345'}});
    if(route.request().method()==='POST'){
     assert.equal(route.request().headers()['x-esp32mic-csrf'],'1');
     saved=Object.fromEntries(new URLSearchParams(route.request().postData()));return route.fulfill({json:{ok:true,reboot_required:false}});
    }
    return route.fulfill({json:info});
   });
   await page.goto('http://battery.test/');await page.waitForFunction(()=>!document.getElementById('choices').disabled);
   assert.equal(await page.locator('#battery_enabled').isChecked(),false);
   await page.check('#battery_enabled');assert(await page.locator('#battery_dialog').isVisible());
   assert.match(await page.locator('#battery_wiring').textContent(),/100 kΩ/);
   assert.match(await page.locator('#battery_dialog').textContent(),/parallel \(1S, 4\.2 V max\)/);
   assert.doesNotMatch(await page.locator('#battery_dialog').textContent(),/1%|100 nF|capacitor/i);
   if(family==='C5'){
    assert.match(await page.locator('#battery_wiring').textContent(),/Ready to use.*built-in/);
    assert.equal(await page.locator('#battery_diagram').isVisible(),false);
   }else{
    assert.match(await page.locator('#battery_wiring').textContent(),new RegExp('GPIO'+(family==='C3'?2:family==='S3'?1:0)));
    assert(await page.locator('#battery_diagram').isVisible());
   }
   for(const theme of ['light','dark']){
    await page.evaluate(t=>document.documentElement.dataset.theme=t,theme);
    const fits=await page.locator('#battery_dialog').evaluate(el=>el.scrollWidth<=el.clientWidth&&el.getBoundingClientRect().right<=innerWidth);
    assert(fits,'battery wiring fits mobile dialog');
    if(!packed&&(family==='C5'||family==='C6'))await page.screenshot({path:`tmp/battery-monitoring/wiring-${family}-${theme}.png`});
   }
   await page.keyboard.press('Escape');assert(await page.locator('#battery_enabled').isChecked());
   await page.evaluate(()=>poll());assert(await page.locator('#battery_enabled').isChecked(),'poll preserves unsaved choice');
   await page.click('#battery_help');assert(await page.locator('#battery_dialog').isVisible());await page.click('#battery_close');
   await page.click('#finish');await page.locator('#done').waitFor();assert.equal(saved.battery,'1');
   info.battery_enabled=true;await page.reload();await page.waitForFunction(()=>!document.getElementById('choices').disabled);
   assert(await page.locator('#battery_enabled').isChecked());assert.equal(await page.locator('#battery_dialog').isVisible(),false);
   if(family==='S3'){
    await page.selectOption('#variant','1');await page.click('#battery_help');assert.match(await page.locator('#battery_board').textContent(),/Sense/);await page.click('#battery_close');
    await page.selectOption('#variant','2');assert(await page.locator('#battery_enabled').isDisabled());assert.equal(await page.locator('#battery_enabled').isChecked(),false);
    await page.selectOption('#variant','0');assert(await page.locator('#battery_enabled').isEnabled());
   }
   await page.uncheck('#battery_enabled');await page.click('#finish');await page.locator('#done').waitFor();assert.equal(saved.battery,'0');
   await page.close();
  }
  for(const packed of [false,true]){
   const page=await browser.newPage({viewport:{width:1280,height:900}});
   page.on('pageerror',e=>errors.push(e.message));
   const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
   fixture.status.battery={enabled:false,supported:true,valid:false,voltage:null,percent:null};
   fixture.status.fw_version='1.36';
   fixture.status.recording={mode:'streaming',supported:true,state:'ready',busy:false,card_ready:false,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:0,dropped_samples:0,file:'',error:''};
   const batteryWrites=[];
   await page.route('http://battery.test/**',route=>{
    const path=new URL(route.request().url()).pathname;
    if(path==='/api/battery'){
     assert.equal(route.request().headers()['x-esp32mic-csrf'],'1');
     const data=Object.fromEntries(new URLSearchParams(route.request().postData()));batteryWrites.push(data);
     if(data.action==='enable')fixture.status.battery.enabled=data.enabled==='1';
     else {fixture.status.battery.calibration_percent=data.action==='reset'?0:1.23;fixture.status.battery.voltage=data.action==='reset'?4.05:4.10;}
     return route.fulfill({json:{ok:true,battery:fixture.status.battery}});
    }
    if(path==='/')return route.fulfill({contentType:'text/html',body:asset('index.html','WEBUI_INDEX_GZ',packed)});
    if(path==='/api/logs')return route.fulfill({body:''});
    return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
   });
   await page.goto('http://battery.test/');await page.waitForFunction(()=>document.getElementById('overview_uptime').textContent.includes(':'));
   assert.equal(await page.locator('#overview_battery').isVisible(),false);
   await page.setViewportSize({width:320,height:850});
   await page.locator('#batterysec > summary').click();
   await page.evaluate(()=>document.querySelectorAll('#batterysec details').forEach(e=>e.open=true));
   for(const family of ['C3','S3','C5','C6','Atom']){
    fixture.status.chip_family='ESP32-'+(family==='Atom'?'S3':family);
    fixture.status.board_name=family==='Atom'?'M5Stack AtomS3U':'XIAO ESP32-'+family;
    fixture.status.battery={enabled:false,supported:family!=='Atom',valid:false,voltage:null,percent:null,calibration_percent:0};
    await page.evaluate(()=>loadStatus());
    assert(await page.locator('#batterysec').isVisible());
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'wiring fits mobile '+family);
    assert.equal(await page.locator('#battery_divider').isVisible(),!['C5','Atom'].includes(family));
    if(family==='Atom'){
     assert.match(await page.locator('#battery_wiring').textContent(),/unsupported/i);
     assert(await page.locator('#battery_monitor').isDisabled());continue;
    }
    assert.match(await page.locator('#battery_wiring').textContent(),family==='C5'?/No external divider.*BAT/:/100 kΩ/);
    const before=batteryWrites.length;
    await page.check('#battery_monitor');assert.equal(await page.locator('#battery_dirty').textContent(),'Unsaved');
    await page.evaluate(()=>loadStatus());assert(await page.locator('#battery_monitor').isChecked());
    await page.uncheck('#battery_monitor');assert.equal(await page.locator('#battery_dirty').textContent(),'');assert(await page.locator('#battery_save').isDisabled());assert.equal(batteryWrites.length,before);
   }
   fixture.status.chip_family='ESP32-C5';fixture.status.board_name='XIAO ESP32-C5';
   fixture.status.battery={enabled:false,supported:true,valid:true,voltage:4.05,raw_voltage:4.05,percent:80,calibration_percent:0};await page.evaluate(()=>loadStatus());
   await page.check('#battery_monitor');await page.click('#battery_save');
   await page.waitForFunction(()=>!batteryPending);assert.equal(batteryWrites.at(-1).enabled,'1');
   assert(await page.locator('#battery_calibrate').isEnabled());
   await page.fill('#battery_reference','4.100');await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#battery_reference').inputValue(),'4.100');
   await page.click('#battery_calibrate');await page.waitForFunction(()=>!batteryPending);
   assert.equal(batteryWrites.at(-1).reference,'4.1');assert.match(await page.locator('#battery_correction').textContent(),/\+1.23%/);
   await page.click('#battery_reset');await page.waitForFunction(()=>!batteryPending);assert.match(await page.locator('#battery_correction').textContent(),/\+0.00%/);
   await page.setViewportSize({width:320,height:850});await page.locator('#batterysec').scrollIntoViewIfNeeded();
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   if(!packed)await page.screenshot({path:'tmp/battery-monitoring/settings-c5-mobile.png'});
   fixture.status.battery={enabled:true,supported:true,valid:true,voltage:3.85,percent:50};
   for(const mode of ['streaming','recording'])for(const theme of ['light','dark'])for(const width of [320,390,1280]){
    fixture.status.recording.mode=mode;await page.setViewportSize({width,height:900});
    await page.evaluate(async t=>{document.documentElement.dataset.theme=t;await loadStatus();},theme);
    assert.equal(await page.locator('#overview_battery').textContent(),'Battery: ~50% · 3.85 V');
    assert.equal(await page.locator('#overview_battery').evaluate(el=>el.tagName),'SPAN');
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
    if(!packed&&(width===320||width===1280))await page.screenshot({path:`tmp/battery-monitoring/overview-${mode}-${theme}-${width}.png`});
   }
   fixture.status.battery.valid=false;await page.evaluate(()=>loadStatus());
   assert.equal(await page.locator('#overview_battery').textContent(),'Battery: unavailable');
   fixture.status.battery={enabled:true,supported:true,valid:true,voltage:3.5,percent:5};await page.evaluate(()=>loadStatus());
   assert.equal(await page.locator('#overview_battery').getAttribute('class'),'bad');
   fixture.status.battery.enabled=false;await page.evaluate(()=>loadStatus());assert.equal(await page.locator('#overview_battery').isVisible(),false);
   await page.close();
  }
  assert.deepEqual(errors,[]);console.log('Battery setup, persistence payloads, board wiring, main overview, modes, themes and mobile/desktop passed (source and embedded assets).');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
