const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});const errors=[];
 try{for(const packed of [false,true]){
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8'),a=h.indexOf('WEBUI_INDEX_GZ[]');source=zlib.gunzipSync(Buffer.from([...h.slice(a,h.indexOf('};',a)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));const page=await browser.newPage({viewport:{width:390,height:900}});page.on('pageerror',e=>errors.push(e.message));let gets=0,moved=false;
  await page.route('http://paging.test/**',route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const url=new URL(route.request().url()),path=url.pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(path==='/api/logs'){
    ++gets;let first=0,last=6000,start=4000,end=6000,text='Latest log messages';
    if(url.searchParams.has('before')){end=Number(url.searchParams.get('before'));start=Math.max(0,end-2000);text=start?'Middle log messages':'Oldest log messages';}
    if(url.searchParams.has('after')){start=Number(url.searchParams.get('after'));end=Math.min(last,start+2000);text=end===last?'Latest log messages':'Middle log messages';}
    if(moved){first=3000;last=9000;start=7000;end=9000;text='Available messages after history changed';}
    return route.fulfill({contentType:'text/plain',headers:{'X-ESP32MIC-Log-First':String(first),'X-ESP32MIC-Log-Last':String(last),'X-ESP32MIC-Log-Start':String(start),'X-ESP32MIC-Log-End':String(end),'X-ESP32MIC-Log-Capacity':'32768','X-ESP32MIC-Log-Storage':'psram','X-ESP32MIC-Log-Moved':moved?'1':'0'},body:text});
   }
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  await page.goto('http://paging.test/');await page.locator('#logssec > summary').click();await page.evaluate(()=>loadLatestLogs());
  assert.equal(await page.locator('#logs').textContent(),'Latest log messages');assert.match(await page.locator('#log_page_status').textContent(),/32 KiB.*PSRAM/);assert(await page.locator('#b_log_older').isEnabled());assert(await page.locator('#b_log_newer').isDisabled());
  await page.click('#b_log_older');await page.waitForFunction(()=>document.getElementById('logs').textContent==='Middle log messages');
  const beforePoll=gets;await page.evaluate(()=>loadAll(true));assert.equal(gets,beforePoll,'polling must not replace the page being read');
  await page.click('#b_log_older');await page.waitForFunction(()=>document.getElementById('logs').textContent==='Oldest log messages');assert(await page.locator('#b_log_older').isDisabled());assert(await page.locator('#b_log_newer').isEnabled());
  await page.click('#b_log_newer');await page.waitForFunction(()=>document.getElementById('logs').textContent==='Middle log messages');
  await page.click('#b_log_latest');await page.waitForFunction(()=>document.getElementById('logs').textContent==='Latest log messages');assert(await page.evaluate(()=>logLatestPage));
  moved=true;await page.evaluate(()=>loadLatestLogs());assert.match(await page.locator('#log_page_status').textContent(),/History changed/);
  for(const width of [390,320]){await page.setViewportSize({width,height:900});assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);}
  fs.mkdirSync('tmp/larger-ui-log',{recursive:true});await page.locator('#logssec').screenshot({path:`tmp/larger-ui-log/paging-${packed?'packed':'source'}.png`});await page.close();
 }assert.deepEqual(errors,[]);console.log('Log paging source/packed: older/newer/latest, frozen older page, overwritten cursor notice and mobile layout passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
