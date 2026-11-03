// Read-only real-device UI verification. All non-GET requests are blocked.
const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
(async()=>{
 const url=process.argv[2];if(!url)throw Error('Usage: NODE_PATH=tools/node_modules node tools/check_live_ui.cjs http://DEVICE');
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 const results=[],errors=[],writes=[];let credentialReadsBlocked=0;
 fs.mkdirSync('tmp/qualification/live-ui-images',{recursive:true});
 try{
  for(const width of [320,390,1280])for(const theme of ['light','dark']){
   const page=await browser.newPage({viewport:{width,height:900}});
   page.on('pageerror',error=>errors.push(error.message));
   await page.route('**/*',route=>{
    const request=route.request(),path=new URL(request.url()).pathname;
    if(request.method()==='POST'&&path==='/api/setup/access'){credentialReadsBlocked++;return route.fulfill({status:403,json:{ok:false,error:'Credential read omitted during validation'}});}
    if(request.method()!=='GET'){writes.push(path);return route.abort();}
    return route.continue();
   });
   await page.goto(url);await page.waitForFunction(()=>document.getElementById('overview_mic').textContent==='Microphone: OK');
   await page.selectOption('#themeSel',theme);
   await page.evaluate(()=>openSection('batterysec'));
   const control=page.locator('#battery_monitor'),saved=await control.inputValue();
   assert(await control.isEnabled());await control.selectOption(saved==='1'?'0':'1');
   assert(await control.evaluate(e=>e.classList.contains('dirty')));
   await page.evaluate(()=>refreshAll());assert.equal(await control.inputValue(),saved==='1'?'0':'1');
   const style=await control.evaluate(e=>({border:getComputedStyle(e).borderColor,font:getComputedStyle(e).fontSize}));
   assert.match(style.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
   await page.locator('#batterysec').screenshot({path:`tmp/qualification/live-ui-images/battery-${width}-${theme}.png`});
   await control.selectOption(saved);assert(!await control.evaluate(e=>e.classList.contains('dirty')));
   await control.selectOption(saved==='1'?'0':'1');await page.locator('#batterysec>[data-discard-drafts]').click();
   await page.waitForFunction(saved=>document.getElementById('battery_monitor').value===saved,saved);
   assert(!await control.evaluate(e=>e.classList.contains('dirty')));
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
   results.push({width,theme,style,ok:true});await page.close();
  }
  assert.deepEqual(errors,[]);assert.deepEqual(writes,[]);
  fs.mkdirSync('tmp/qualification',{recursive:true});
  fs.writeFileSync('tmp/qualification/live-ui.json',JSON.stringify({ok:true,results,errors,writes,credentialReadsBlocked},null,2));
  console.log('Live device UI: 320/390/1280 px, both themes, orange drafts, polling/revert/discard and zero writes passed');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
