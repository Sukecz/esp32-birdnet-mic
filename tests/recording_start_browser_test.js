// Verify actionable start failures and retry in source and embedded UI assets.
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const results=[];fs.mkdirSync('tmp/recording-ram-fix/browser',{recursive:true});
 try{for(const packed of [false,true]){
  let html=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');html=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  for(const width of [320,390,1280])for(const theme of ['light','dark']){
   const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
   Object.assign(fixture.status,{mdns_enabled:true,recording:{mode:'recording',state:'ready',supported:true,busy:false,cooling_pending:false,card_ready:true,elapsed_seconds:0,duration_minutes:0,segment_minutes:60,auto_start:false,written_kib:0,free_mib:30711,dropped_samples:0,file:'',error:''}});
   const error='Not enough internal RAM for SD; stop Listen and close extra device tabs';
   let fail=true;const errors=[],posts=[];
   const page=await browser.newPage({viewport:{width,height:900}});
   page.on('pageerror',e=>errors.push(e.message));
   await page.route('http://ui.test/**',async route=>{
    const req=route.request(),path=new URL(req.url()).pathname;
    if(path==='/ap-password.js')return require('./ui_assets').apPassword(route,packed);
    if(path==='/')return route.fulfill({contentType:'text/html',body:html});
    if(req.method()==='POST'){
     assert.equal(req.headers()['x-esp32mic-csrf'],'1');posts.push(path);
     assert.equal(path,'/api/recording/action');assert.equal(new URLSearchParams(req.postData()).get('action'),'start');
     Object.assign(fixture.status.recording,fail?{state:'error',error}:{state:'recording',busy:true,error:'',written_kib:3777,elapsed_seconds:40});
     return route.fulfill({json:fail?{ok:false,error}:{ok:true}});
    }
    if(path==='/api/logs')return route.fulfill({body:''});
    return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
   });
   await page.goto('http://ui.test/');await page.waitForSelector('#recording_panel:visible');
   await page.selectOption('#themeSel',theme);await page.click('#recording_start');
   await page.waitForFunction(()=>document.querySelector('#recording_feedback').textContent.startsWith('Not enough internal RAM'));
   assert.equal(await page.locator('#recording_feedback').textContent(),error);
   assert.match(await page.locator('#recording_summary').textContent(),/Not enough internal RAM/);
   assert(await page.locator('#recording_start').isEnabled());
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   const typography=await page.locator('#recording_feedback').evaluate(e=>({font:getComputedStyle(e).fontSize,weight:getComputedStyle(e).fontWeight,color:getComputedStyle(e).color}));
   await page.screenshot({path:`tmp/recording-ram-fix/browser/${packed?'embedded':'source'}-${width}-${theme}.png`,fullPage:true});
   fail=false;await page.click('#recording_start');await page.waitForFunction(()=>document.querySelector('#device_mode_status').textContent==='Recording: OK');
   assert.equal(await page.locator('#recording_feedback').textContent(),'');assert(await page.locator('#recording_start').isDisabled());
   assert.equal(fixture.status.mdns_enabled,true);assert.equal(posts.length,2);assert.deepEqual(errors,[]);
   results.push({packed,width,theme,typography});await page.close();
  }
 }
 fs.writeFileSync('tmp/recording-ram-fix/browser/results.json',JSON.stringify(results,null,2));
 console.log('Recording start: specific error, retry, mDNS preserved, source/embedded, 320/390/1280 px and light/dark passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
