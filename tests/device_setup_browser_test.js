// NODE_PATH=tmp/browser-tools/node_modules node tests/device_setup_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
function asset(name,symbol,packed){
 if(!packed)return fs.readFileSync('esp32-birdnet-mic/webui/'+name,'utf8');
 const header=fs.readFileSync('esp32-birdnet-mic/SetupAssets_gz.h','utf8');
 const data=header.slice(header.indexOf(symbol+'[]'),header.indexOf('};',header.indexOf(symbol+'[]')));
 return zlib.gunzipSync(Buffer.from([...data.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
}
const base={ok:true,complete:false,default_mode:'streaming',connection:'home',connected:false,ssid:'',ap_name:'BirdNETMic-A1B2C3',ap_active:true,ap_ip:'192.168.4.1',ip:'0.0.0.0',test_state:'idle',test_error:'',chip_family:'ESP32-C6',board_name:'XIAO ESP32-C6',s3_board_supported:false,s3_board:0,antenna_supported:true,antenna:'external'};
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 try{
  for(const packed of [false,true]){
   for(const width of [320,390,1000]){
    const page=await browser.newPage({viewport:{width,height:850},colorScheme:width===390?'dark':'light'}),errors=[],saves=[];
    let state={...base},trial='',scanCalls=0,scanReads=0;
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('http://setup.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
     const req=route.request(),path=new URL(req.url()).pathname;
     for(const [url,name,symbol,type] of [['/','setup.html','SETUP_HTML_GZ','text/html'],['/setup.css','setup.css','SETUP_CSS_GZ','text/css'],['/setup.js','setup.js','SETUP_JS_GZ','application/javascript']]){
      if(path===url)return route.fulfill({contentType:type,body:asset(name,symbol,packed)});
     }
     if(req.method()==='POST')assert.equal(req.headers()['x-esp32mic-csrf'],'1');
     const body=new URLSearchParams(req.postData()||'');let json={ok:true};
     if(path==='/api/setup'&&req.method()==='GET')json=state;
     else if(path==='/api/setup/access')json={ok:true,password:'ABCD2345'};
     else if(path==='/api/setup/scan'){
      if(req.method()==='POST'){scanCalls++;return route.abort('failed');}
      else if(++scanReads===1)return route.abort('failed');
      else if(scanReads===2)json={ok:true,scanning:true};
      else json={ok:true,scanning:false,aps:[{ssid:'Home <safe>',secure:true},{ssid:'Home <safe>',secure:true},{ssid:'',secure:false}]};
     }else if(path==='/api/setup/connect'){trial=body.get('ssid');state.test_state='connecting';}
     else if(path==='/api/setup'){saves.push(Object.fromEntries(body));json={ok:true,reboot_required:false};}
     else throw Error('Unexpected request: '+path);
     return route.fulfill({json});
    });
    await page.goto('http://setup.test/');
    await page.waitForFunction(()=>!document.getElementById('choices').disabled);
    assert.equal(await page.locator('#variant_field').isVisible(),false);
    assert.equal(await page.locator('#antenna_field').isVisible(),true);
    assert.equal(await page.locator('#antenna').inputValue(),'external');
    await page.selectOption('#mode','recording');
    assert.equal(await page.locator('#connection_field').isVisible(),true);
    assert.match(await page.locator('#fallback').textContent(),/Recording continues offline/);
    await page.click('#scan');
    await page.waitForFunction(()=>!document.getElementById('scan').disabled);
    assert.equal(scanCalls,1);assert.equal(await page.locator('#networks button').count(),1);
    assert.equal(await page.locator('#networks button span').textContent(),'Home <safe>');
    assert.equal(await page.locator('#networks').isVisible(),true);
    await page.locator('#networks button').click();
    assert.equal(await page.locator('#ssid').inputValue(),'Home <safe>');
    assert.equal(await page.locator('#networks button').getAttribute('aria-pressed'),'true');await page.fill('#password','wrong-test-password');
    assert.equal(await page.locator('#finish').isDisabled(),true);
    await Promise.all([page.waitForResponse(r=>new URL(r.url()).pathname==='/api/setup/connect'),page.click('#connect')]);assert.equal(trial,'Home <safe>');
    assert(await page.locator('#ssid').isDisabled());assert(await page.locator('#password').isDisabled());
    state.test_state='error';await page.waitForFunction(()=>!polling);await page.evaluate(()=>poll());
    assert.match(await page.locator('#wifi_status').textContent(),/Could not connect/);
    assert(await page.locator('#ssid').isEnabled());assert(await page.locator('#password').isEnabled());
    assert.equal(await page.locator('#ssid').inputValue(),'Home <safe>');
    await page.fill('#password','correct-test-password');await Promise.all([page.waitForResponse(r=>new URL(r.url()).pathname==='/api/setup/connect'),page.click('#connect')]);
    state={...state,test_state:'connected',connected:true,ip:'192.168.1.123'};
    await page.waitForFunction(()=>!polling);await page.evaluate(()=>poll());assert.equal(await page.locator('#finish').isDisabled(),false);
    assert.equal(await page.locator('#password').inputValue(),'');
    // Editing after a successful trial must require another test before save.
    assert(await page.locator('#ssid').isEnabled());assert(await page.locator('#password').isEnabled());
    await page.fill('#ssid','Edited network B');assert(await page.locator('#finish').isDisabled());
    await page.fill('#ssid','Home <safe>');await page.fill('#password','correct-test-password');
    await Promise.all([page.waitForResponse(r=>new URL(r.url()).pathname==='/api/setup/connect'),page.click('#connect')]);assert(await page.locator('#ssid').isDisabled());
    state.test_state='connected';await page.waitForFunction(()=>!polling);await page.evaluate(()=>poll());
    assert.equal(await page.locator('#finish').isDisabled(),false);
    await page.waitForFunction(()=>!polling);await page.evaluate(()=>poll());assert.equal(scanCalls,1);
    assert.equal(await page.locator('#mode').inputValue(),'recording');
    const fit=await page.evaluate(()=>({page:document.documentElement.scrollWidth,viewport:innerWidth,heights:[...document.querySelectorAll('button,select,input')].filter(e=>e.getClientRects().length).map(e=>(e.type==='checkbox'?e.closest('label'):e).getBoundingClientRect().height)}));
    assert(fit.page<=fit.viewport);assert(fit.heights.every(h=>h>=44));
    if(!packed&&width===320)await page.screenshot({path:'tmp/device-setup-mobile.png',fullPage:true});
    await page.click('#finish');
    await page.locator('#done').waitFor({state:'visible'});
    assert.equal(saves.length,1);assert.equal(saves[0].mode,'recording');assert.equal(saves[0].connection,'home');
    assert.equal(saves[0].antenna,'external');
    assert.equal(await page.locator('#done').isVisible(),true);
    assert.match(await page.locator('#done_access').textContent(),/require this password/);
    assert.equal(await page.locator('#open_controls').getAttribute('href'),'http://192.168.1.123/');
    assert.deepEqual(errors,[]);await page.close();
   }
  }
  // Reload during a trial, S3 capability gating and standalone setup.
  const page=await browser.newPage({viewport:{width:320,height:850}});let state={...base,s3_board_supported:true,antenna_supported:false,board_name:'XIAO ESP32-S3',test_state:'connecting'},save;
  await page.addInitScript(()=>Object.defineProperty(window,'localStorage',{get(){throw Error('Unavailable in captive browser');}}));
  await page.route('http://setup.test/**',route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:asset('setup.html','',false)});
   if(path==='/setup.js'||path==='/setup.css')return route.fulfill({contentType:path.endsWith('js')?'application/javascript':'text/css',body:asset(path.slice(1),'',false)});
   if(path==='/api/setup/access')return route.fulfill({json:{ok:true,password:'ABCD2345'}});
   if(route.request().method()==='POST'){save=Object.fromEntries(new URLSearchParams(route.request().postData()));return route.fulfill({json:{ok:true,reboot_required:false}});}
   return route.fulfill({json:state});
  });
  await page.goto('http://setup.test/');await page.waitForFunction(()=>!document.getElementById('choices').disabled);
  assert.equal(await page.locator('#finish').isDisabled(),true);state.test_state='error';await page.waitForFunction(()=>!polling);await page.evaluate(()=>poll());
  assert.equal(await page.locator('#antenna_field').isVisible(),false);
  await page.selectOption('#variant','2');assert.equal(await page.locator('#mode option[value=recording]').isDisabled(),true);
  await page.selectOption('#variant','1');await page.selectOption('#mode','recording');await page.selectOption('#connection','direct');
  assert.equal(await page.locator('#home_fields').isVisible(),false);assert.equal(await page.locator('#later').isVisible(),false);
  await page.click('#finish');await page.locator('#done').waitFor({state:'visible'});assert.equal(save.variant,'1');assert.equal(save.connection,'direct');
  assert.equal(await page.locator('#open_controls').getAttribute('href'),'http://192.168.4.1/');await page.close();
  // A delayed poll from the previous failed trial must not unlock a new test.
  const racePage=await browser.newPage();let holdNext=false,releaseStatus,requestHeld;
  const held=new Promise(resolve=>{requestHeld=resolve;});
  let raceState={...base,complete:true,ssid:'Original',test_state:'error',test_error:'connection_failed'};
  await racePage.route('http://setup.test/**',route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/'||path==='/setup.js'||path==='/setup.css')return route.fulfill({contentType:path.endsWith('.js')?'application/javascript':path.endsWith('.css')?'text/css':'text/html',body:asset(path==='/'?'setup.html':path.slice(1),'',false)});
   if(path==='/api/setup/access')return route.fulfill({json:{ok:true,password:'ABCD2345'}});
   if(path==='/api/setup/connect'){raceState={...raceState,test_state:'queued'};return route.fulfill({json:{ok:true}});}
   if(holdNext){
    holdNext=false;const previous={...raceState};releaseStatus=()=>route.fulfill({json:previous});requestHeld();return;
   }
   return route.fulfill({json:raceState});
  });
  await racePage.goto('http://setup.test/');
  await racePage.waitForFunction(()=>!document.getElementById('choices').disabled&&!polling);
  await racePage.fill('#ssid','New network');await racePage.fill('#password','new-password');
  holdNext=true;const pendingPoll=racePage.evaluate(()=>poll());await held;
  await Promise.all([racePage.waitForResponse(r=>new URL(r.url()).pathname==='/api/setup/connect'),racePage.click('#connect')]);
  await releaseStatus();await pendingPoll;
  assert(await racePage.locator('#ssid').isDisabled());assert(await racePage.locator('#password').isDisabled());
  assert(await racePage.locator('#finish').isDisabled());
  raceState={...raceState,test_state:'connected',connected:true};
  await racePage.waitForFunction(()=>!polling);await racePage.evaluate(()=>poll());
  assert(await racePage.locator('#ssid').isEnabled());assert(await racePage.locator('#finish').isEnabled());
  await racePage.close();
  console.log('Device setup: source/embedded assets, mobile sizing, board gating, scan, failed/retried Wi-Fi, locked credentials, stale polls, drafts, reload, standalone and access instructions passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
