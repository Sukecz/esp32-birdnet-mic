// NODE_PATH=tmp/browser-tools/node_modules node tests/antenna_browser_test.js
const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
(async()=>{
 const browser=await chromium.launch({executablePath:'/snap/bin/chromium',headless:true,args:['--no-sandbox']});
 const source=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8');
 try{
  const page=await browser.newPage(),writes=[],errors=[];
  let accept=false,rejectSave=false;
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>accept?d.accept():d.dismiss());
  await page.route('http://ui.test/**',async route=>{
   const request=route.request(),path=new URL(request.url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:source});
   if(path==='/api/settings/antenna'){
    assert.equal(request.headers()['x-esp32mic-csrf'],'1');
    const value=new URLSearchParams(request.postData()).get('antenna');writes.push(value);
    return route.fulfill({json:rejectSave?{ok:false,error:'antenna_save_failed'}:{ok:true,reboot_required:true}});
   }
   if(path==='/api/logs')return route.fulfill({body:''});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false,latest:'1.36'}});
  });
  for(const board of ['xiao-esp32c3','xiao-esp32s3','xiao-esp32c5']){
   fixture.status.board_id=board;fixture.status.wifi_antenna_supported=false;fixture.status.wifi_antenna=null;
   await page.goto('http://ui.test/');await page.waitForFunction(()=>document.getElementById('ip').textContent!=='');
   await page.locator('#networksec > summary').click();assert(await page.locator('#row_wifi_antenna').isHidden());
   await page.setViewportSize({width:390,height:844});assert(await page.locator('#row_wifi_antenna').isHidden());
   await page.setViewportSize({width:1280,height:900});
  }
  fixture.status.board_id='xiao-esp32c6';fixture.status.wifi_antenna_supported=true;fixture.status.wifi_antenna='external';
  await page.goto('http://ui.test/');await page.waitForFunction(()=>document.getElementById('row_wifi_antenna').hidden===false);
  await page.locator('#networksec > summary').click();assert(await page.locator('#row_wifi_antenna').isVisible());
  assert.equal(await page.locator('#wifi_antenna').inputValue(),'external');assert(await page.locator('#wifi_antenna_save').isDisabled());
  await page.selectOption('#wifi_antenna','internal');await page.evaluate(()=>refreshAll());
  assert.equal(await page.locator('#wifi_antenna').inputValue(),'internal');assert.equal(await page.locator('#wifi_antenna_dirty').textContent(),'Unsaved');
  await page.selectOption('#wifi_antenna','external');assert(await page.locator('#wifi_antenna_save').isDisabled());assert.equal(writes.length,0);
  await page.selectOption('#wifi_antenna','internal');await page.click('#wifi_antenna_save');assert.equal(writes.length,0); // Cancel leaves RF untouched.
  await page.evaluate(()=>{window.rebootCalls=0;window.rebootSequence=()=>{window.rebootCalls++;};});
  accept=true;rejectSave=true;await page.click('#wifi_antenna_save');await page.waitForFunction(()=>!antennaBusy);
  assert.equal(await page.locator('#wifi_antenna').inputValue(),'internal');assert.equal(await page.locator('#wifi_antenna_dirty').textContent(),'Unsaved');
  assert.equal(await page.evaluate(()=>window.rebootCalls),0);
  rejectSave=false;await page.click('#wifi_antenna_save');await page.waitForFunction(()=>window.rebootCalls===1);
  assert.deepEqual(writes,['internal','internal']);assert.deepEqual(errors,[]);
  console.log('Antenna UI (mock device only): C6 visibility/default, drafts/revert, confirmation, failed save and restart passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
