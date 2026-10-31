// Real Web Audio lifecycle, temporary busy retries and cancellation in both assets.
const assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox','--autoplay-policy=no-user-gesture-required']});
const results=[];fs.mkdirSync('tmp/listen-c6-fix/browser',{recursive:true});
try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
 let html=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
 if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');html=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
 const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json','utf8')),errors=[];
 const page=await browser.newPage({viewport:{width,height:900}});page.on('pageerror',e=>errors.push(e.message));
 await page.addInitScript(()=>{
  const original=window.fetch;window.__listenMock={statuses:[],calls:0,streams:[]};
  window.fetch=async(url,options)=>{
   if(!String(url).includes(':81/'))return original(url,options);
   const mock=window.__listenMock;mock.calls++;const status=mock.statuses.shift()??200;
   if(status===503)return new Response(null,{status});
   const bytes=new Uint8Array(44+2048),view=new DataView(bytes.buffer);
   bytes.set(new TextEncoder().encode('RIFF'));view.setUint16(20,1,true);view.setUint16(22,1,true);view.setUint32(24,48000,true);view.setUint16(34,16,true);
   const body=new ReadableStream({start(controller){mock.streams.push(controller);controller.enqueue(bytes);options.signal.addEventListener('abort',()=>{try{controller.error(new DOMException('Aborted','AbortError'));}catch(_){}},{once:true});}});
   return new Response(body,{status,headers:{'Content-Type':'audio/wav'}});
  };
 });
 await page.route('http://ui.test/**',async route=>{
  const req=route.request(),path=new URL(req.url()).pathname;
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/ap-password.js')return require('./ui_assets').apPassword(route,packed);
  if(req.method()==='POST')return route.fulfill({json:{ok:true,password:'EXAMPLE12'}});
  if(path==='/api/logs')return route.fulfill({body:''});
  return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
 });
 await page.goto('http://ui.test/');await page.waitForFunction(()=>document.getElementById('overview_mic').textContent==='Microphone: OK');await page.selectOption('#themeSel',theme);
 await page.evaluate(()=>{__listenMock.statuses=[503,503,200];});await page.click('#listen_button');
 await page.waitForFunction(()=>document.getElementById('listen_status').textContent==='Listening');
 assert.equal(await page.evaluate(()=>__listenMock.calls),3);assert.equal(await page.evaluate(()=>listenSession.context.state),'running');
 await page.evaluate(()=>{window.originalListenContext=listenSession.context;setListenVolume(125);__listenMock.streams.at(-1).close();});
 await page.waitForFunction(()=>document.getElementById('listen_status').textContent==='Reconnecting…');
 await page.waitForFunction(()=>__listenMock.calls===4&&document.getElementById('listen_status').textContent==='Listening');
 assert(await page.evaluate(()=>listenSession.context===originalListenContext&&listenSession.gain.gain.value===1.25));
 if(packed&&width===390&&theme==='dark'){
  await page.waitForFunction(()=>document.getElementById('listen_status').textContent==='Reconnecting…',{timeout:6000});
  await page.waitForFunction(()=>__listenMock.calls===5&&document.getElementById('listen_status').textContent==='Listening');
  assert(await page.evaluate(()=>listenSession.context===originalListenContext&&listenSession.gain.gain.value===1.25));
 }
 await page.click('#listen_button');assert.equal(await page.locator('#listen_button').textContent(),'Listen');
 // Stop while retrying, then start another session before the old delay ends.
 await page.evaluate(()=>{__listenMock.statuses=[503];});await page.click('#listen_button');
 await page.waitForFunction(()=>document.getElementById('listen_status').textContent==='Waiting for Listen…');
 await page.click('#listen_button');await page.click('#listen_button');
 await page.waitForFunction(()=>document.getElementById('listen_status').textContent==='Listening');
 await page.waitForTimeout(300);assert.equal(await page.locator('#listen_status').textContent(),'Listening');
 await page.click('#listen_button');
 await page.evaluate(()=>{__listenMock.statuses=[503,503,503,503];});await page.click('#listen_button');
 await page.waitForFunction(()=>document.getElementById('listen_status').textContent.includes('Stop other listening tabs'));
 assert.equal(await page.evaluate(()=>listenSession),null);assert.equal(await page.locator('#listen_button').getAttribute('aria-pressed'),'false');
 assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
 const typography=await page.locator('#listen_status').evaluate(e=>({font:getComputedStyle(e).fontSize,color:getComputedStyle(e).color}));
 await page.screenshot({path:`tmp/listen-c6-fix/browser/${packed?'embedded':'source'}-${width}-${theme}.png`,fullPage:true});
 assert.deepEqual(errors,[]);results.push({packed,width,theme,typography});await page.close();
}
fs.writeFileSync('tmp/listen-c6-fix/browser/results.json',JSON.stringify(results,null,2));console.log('Listen browser: transient busy, bounded retries, cancel/restart and clear errors; source/embedded, 320/390/1280 px, light/dark passed.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
