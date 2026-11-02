// Read-only real-device UI verification. All non-GET requests are blocked.
const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
(async()=>{
 const url=process.argv[2];if(!url)throw Error('Usage: NODE_PATH=tools/node_modules node tools/check_live_ui.cjs http://DEVICE');
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 const results=[],errors=[],writes=[];
 try{
  for(const width of [320,390,1280])for(const theme of ['light','dark']){
   const page=await browser.newPage({viewport:{width,height:900}});
   page.on('pageerror',error=>errors.push(error.message));
   await page.route('**/*',route=>{
    if(route.request().method()!=='GET'){writes.push(new URL(route.request().url()).pathname);return route.abort();}
    return route.continue();
   });
   await page.goto(url);await page.waitForFunction(()=>document.getElementById('overview_mic').textContent==='Microphone: OK');
   await page.selectOption('#themeSel',theme);
   await page.evaluate(()=>openSection('batterysec'));
   const checkbox=page.locator('#battery_monitor'),saved=await checkbox.isChecked();
   assert(await checkbox.isEnabled());await checkbox.setChecked(!saved);
   assert(await checkbox.evaluate(e=>e.classList.contains('dirty')));
   await page.evaluate(()=>refreshAll());assert.equal(await checkbox.isChecked(),!saved);
   const style=await checkbox.evaluate(e=>({border:getComputedStyle(e.closest('label')).borderColor,font:getComputedStyle(e.closest('label')).fontSize}));
   assert.match(style.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
   await checkbox.setChecked(saved);assert(!await checkbox.evaluate(e=>e.classList.contains('dirty')));
   await checkbox.setChecked(!saved);await page.locator('#batterysec>[data-discard-drafts]').click();
   await page.waitForFunction(saved=>document.getElementById('battery_monitor').checked===saved,saved);
   assert(!await checkbox.evaluate(e=>e.classList.contains('dirty')));
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
   results.push({width,theme,style,ok:true});await page.close();
  }
  assert.deepEqual(errors,[]);assert.deepEqual(writes,[]);
  fs.mkdirSync('tmp/qualification',{recursive:true});
  fs.writeFileSync('tmp/qualification/live-ui.json',JSON.stringify({ok:true,results,errors,writes},null,2));
  console.log('Live UI: source-served device page, 320/390/1280 px, both themes, orange drafts, polling/revert/discard and zero writes passed');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
