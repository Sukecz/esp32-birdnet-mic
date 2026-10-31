const assert=require('node:assert/strict');
const fs=require('node:fs'),zlib=require('node:zlib');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 try{
 for(const packed of [false,true]){
  let source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
  if(packed){const h=fs.readFileSync('esp32-birdnet-mic/WebUI_gz.h','utf8');source=zlib.gunzipSync(Buffer.from([...h.matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();}
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json','utf8'));
  Object.assign(fixture.audio_status,{s3_board_supported:false,s3_board:0,builtin_mic:false});
  let failure=false,pause=false,release,requestStarted;
  const writes=[],errors=[];
  const page=await browser.newPage({viewport:{width:390,height:844}});
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://ui.test/**',async route=>{
   if(new URL(route.request().url()).pathname==='/ap-password.js')return require('./ui_assets').apPassword(route,typeof packed==='undefined'?false:packed);
   const req=route.request(),path=new URL(req.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(req.method()!=='GET'){
    const values=Object.fromEntries(new URLSearchParams(req.postData()));
    writes.push({path,values,csrf:req.headers()['x-esp32mic-csrf']});
    if(pause)await new Promise(resolve=>{release=resolve;requestStarted();});
    if(failure)return route.fulfill({json:{ok:false,error:'i2s_restart_failed'}});
    assert.equal(path,'/api/settings/s3_board');
    const a=fixture.audio_status;a.s3_board=Number(values.variant);a.builtin_mic=a.s3_board!==0;
    if(a.builtin_mic){a.mic_format=2;a.sample_rate=48000;}
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:'Example log'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.31'}});
  });
  await page.goto('http://ui.test/');await page.waitForFunction(()=>document.querySelector('#in_gain').value==='1.50');
  await page.evaluate(()=>openSection('audiosec'));
  assert(!await page.locator('#row_s3_board').isVisible());
  fixture.audio_status.s3_board_supported=true;await page.evaluate(()=>refreshAll());
  assert(await page.locator('#row_s3_board').isVisible());
  assert.equal(await page.locator('#sel_s3_board').inputValue(),'0');
  assert.match(await page.locator('#row_s3_board').textContent(),/Experimental — needs more testing/);
  assert(await page.locator('#row_s3_board').evaluate(e=>e.nextElementSibling.querySelector('#sel_mic_format')!==null));
  for(const width of [320,390,1280]){
   await page.setViewportSize({width,height:844});
   assert(await page.locator('#sel_s3_board').evaluate(e=>{const b=e.getBoundingClientRect();return b.x>=0&&b.right<=innerWidth;}),'board selector overflows');
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'page overflows');
  }
  await page.setViewportSize({width:390,height:844});
  await page.locator('#in_gain').fill('7');
  await page.selectOption('#sel_s3_board','1');await page.evaluate(()=>refreshAll());
  assert.equal(await page.locator('#sel_s3_board').inputValue(),'1');assert.equal(writes.length,0);
  await page.locator('#btn_s3_board_apply').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent.startsWith('Board saved.'));
  assert.deepEqual(writes[0],{path:'/api/settings/s3_board',values:{variant:'1'},csrf:'1'});
  assert.equal(await page.locator('#in_gain').inputValue(),'7');
  await page.waitForFunction(()=>!document.querySelector('#sel_mic_format').disabled&&document.querySelector('#sel_mic_format').value==='2');
  assert(!await page.locator('#sel_mic_format').isDisabled());
  assert(await page.locator('#in_rate').isDisabled());
  assert.equal(await page.locator('#sel_mic_format').inputValue(),'2');
  assert.equal(await page.locator('#sel_s3_board').evaluate(e=>e.classList.contains('dirty')),false);
  // A Sense profile restored with an external I2S microphone stays selectable.
  fixture.audio_status.mic_format=0;fixture.audio_status.builtin_mic=false;
  await page.evaluate(()=>refreshAll());
  await page.waitForFunction(()=>document.querySelector('#sel_mic_format').value==='0'&&!document.querySelector('#in_rate').disabled);
  assert.equal(await page.locator('#sel_s3_board').inputValue(),'1');
  assert(!await page.locator('#sel_mic_format').isDisabled());
  assert.equal(writes.length,1);

  failure=true;await page.selectOption('#sel_s3_board','2');await page.locator('#btn_s3_board_apply').click();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.classList.contains('bad'));
  assert.equal(fixture.audio_status.s3_board,1);
  assert.equal(await page.locator('#sel_s3_board').inputValue(),'2');
  assert(await page.locator('#sel_s3_board').evaluate(e=>e.classList.contains('dirty')));
  failure=false;pause=true;const holding=new Promise(resolve=>requestStarted=resolve);await page.locator('#btn_s3_board_apply').click();await holding;
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent==='Saving…');
  await page.selectOption('#sel_s3_board','0');release();
  await page.waitForFunction(()=>document.querySelector('#audiosec>.feedback')?.textContent.startsWith('Board saved.'));
  await page.waitForFunction(()=>document.querySelector('#sel_mic_format').disabled);
  assert.equal(fixture.audio_status.s3_board,2);assert.equal(await page.locator('#sel_s3_board').inputValue(),'0');
  pause=false;await page.locator('#btn_s3_board_apply').click();
  await page.waitForFunction(()=>!document.querySelector('#sel_mic_format').disabled);
  assert.equal(fixture.audio_status.s3_board,0);
  assert.equal(await page.locator('#in_gain').inputValue(),'7');
  assert.equal(errors.length,0,errors.join('\n'));
  // Show the new control and its experimental note in the local review artifact.
  await page.locator('#row_s3_board').screenshot({path:'tmp/s3-board-selector.png'});
  await page.close();
 }
 console.log('S3 board UI: source/embedded, visibility, mobile layout, selection, failure and pending drafts passed');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
