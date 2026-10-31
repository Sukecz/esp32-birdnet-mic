// NODE_PATH=tmp/browser-tools/node_modules node tests/ui_poll_recovery_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
fixture.status.recording={mode:'streaming',state:'ready',supported:true,busy:false,card_ready:false,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:0,dropped_samples:0,file:'',error:''};
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 try{
  const page=await browser.newPage();let statusCalls=0,logsCalls=0;const held=[];
  await page.route('http://ui.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8')});
   if(path==='/api/status'&&++statusCalls===1)return route.abort();
   if(path==='/api/logs'){logsCalls++;held.push(route);return;}
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await page.goto('http://ui.test/',{waitUntil:'domcontentloaded'});
  // Simulate one failed startup request plus a stalled, unopened log panel.
  await page.waitForFunction(()=>document.getElementById('overview_wifi').textContent.includes('dBm'),null,{timeout:6500});
  assert(statusCalls>=2,'Status must retry after a transient startup failure');
  assert.equal(logsCalls,0,'Closed logs must not gate the first status refresh');
  assert.equal(await page.locator('#connection_status').textContent(),'');
  assert.notEqual(await page.locator('#device_mode_status').textContent(),'Checking…');
  await page.close();
  const logPage=await browser.newPage();
  await logPage.addInitScript(()=>{
   const timeout=AbortSignal.timeout.bind(AbortSignal);window.requestedTimeouts=[];
   AbortSignal.timeout=ms=>{window.requestedTimeouts.push(ms);return timeout(Math.min(ms,400));};
  });
  await logPage.route('http://ui.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8')});
   if(path==='/api/logs'){held.push(route);return;}
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await logPage.goto('http://ui.test/',{waitUntil:'domcontentloaded'});
  await logPage.waitForFunction(()=>document.getElementById('device_mode_status').textContent!=='Checking…');
  const failure=await logPage.evaluate(()=>loadLogs().then(()=>null,e=>e.name));
  assert(['TimeoutError','AbortError'].includes(failure),'A stalled log response must time out');
  assert.equal(await logPage.evaluate(()=>window.requestedTimeouts.at(-1)),8000);
  await logPage.close();
  // Captive/private browsers can deny even reading window.localStorage.
  const restricted=await browser.newPage(),storageErrors=[];let restrictedStatus=0;
  restricted.on('pageerror',error=>storageErrors.push(error.message));
  await restricted.addInitScript(()=>Object.defineProperty(window,'localStorage',{
   get(){throw new DOMException('Storage denied','SecurityError');}
  }));
  await restricted.route('http://ui.test/**',route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8')});
   if(path==='/api/status')++restrictedStatus;
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await restricted.goto('http://ui.test/');
  await restricted.waitForFunction(()=>document.getElementById('overview_wifi').textContent.includes('dBm'));
  assert(restrictedStatus>0);
  await restricted.selectOption('#themeSel','dark');
  assert.equal(await restricted.evaluate(()=>document.documentElement.dataset.theme),'dark');
  assert.deepEqual(storageErrors,[]);await restricted.close();
  console.log('Polling recovery: startup status retry proceeds despite stalled unopened logs');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
