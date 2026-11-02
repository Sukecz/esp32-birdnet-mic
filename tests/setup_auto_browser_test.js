const assert=require('node:assert/strict'),fs=require('node:fs'),assets=require('./ui_assets');
const {chromium}=require('playwright-core');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 const results=[];
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  let info={ok:true,complete:true,s3_board_supported:true,s3_board:0,mic_format:0,board_name:'XIAO ESP32-S3',chip_family:'ESP32-S3',default_mode:'streaming',connection:'home',ssid:'Test network',connected:true,ip:'192.0.2.1',ap_name:'BirdNETMic-Test',ap_active:false,test_state:'idle',battery_enabled:false,battery_supported:true,antenna_supported:false,antenna:'external',detection:{mic_auto:false,board_auto:false,note:'Manual selections retained.'}};
  let fail=false,writes=[];const errors=[];
  const page=await browser.newPage({viewport:{width,height:900}});page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://setup-auto.test/**',route=>{
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:assets.setup('setup.html','SETUP_HTML_GZ',packed)});
   if(path==='/setup.js'||path==='/setup.css')return route.fulfill({contentType:path.endsWith('js')?'application/javascript':'text/css',body:assets.setup(path.slice(1),path.endsWith('js')?'SETUP_JS_GZ':'SETUP_CSS_GZ',packed)});
   if(path==='/ap-password.js')return assets.apPassword(route,packed);
   if(path==='/api/setup/access')return route.fulfill({json:{ok:true,password:'TEST2345'}});
   if(route.request().method()==='POST'){
    writes.push(Object.fromEntries(new URLSearchParams(route.request().postData())));
    return route.fulfill({status:fail?500:200,json:fail?{ok:false,error:'Save failed. Your settings are unchanged.'}:{ok:true,reboot_required:false}});
   }
   return route.fulfill({json:info});
  });
  await page.goto('http://setup-auto.test/');await page.waitForFunction(()=>!document.getElementById('choices').disabled);
  await page.evaluate(theme=>document.documentElement.dataset.theme=theme,theme);
  const dirty=async id=>page.locator('#'+id).evaluate(e=>e.classList.contains('dirty'));
  for(const id of ['variant','microphone']){
   assert.equal(await page.inputValue('#'+id),'0');
   await page.selectOption('#'+id,'3');assert(await dirty(id));
   await page.evaluate(()=>poll());assert.equal(await page.inputValue('#'+id),'3');
   await page.selectOption('#'+id,'0');assert(!await dirty(id));
  }
  await page.click('#finish');assert.equal(writes.length,0);
  await page.selectOption('#variant','3');await page.selectOption('#microphone','3');
  await page.click('#discard_choices');assert.equal(await page.inputValue('#variant'),'0');assert(!await dirty('microphone'));
  await page.selectOption('#variant','3');await page.selectOption('#microphone','3');
  fail=true;await page.click('#finish');await page.waitForFunction(()=>!working);
  assert(await dirty('variant'));assert(await dirty('microphone'));assert.match(await page.locator('#feedback').textContent(),/failed/);
  const style=await page.locator('#microphone').evaluate(e=>({font:getComputedStyle(e).fontSize,reference:getComputedStyle(document.getElementById('variant')).fontSize,border:getComputedStyle(e).borderColor}));
  assert.equal(style.font,style.reference);assert.match(style.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  if(packed)await page.screenshot({path:`tmp/qualification/auto-ui/setup-${width}-${theme}.png`,fullPage:true});
  fail=false;await page.click('#finish');await page.locator('#done').waitFor();
  assert.equal(writes.at(-1).variant,'3');assert.equal(writes.at(-1).microphone,'3');
  info.detection.mic_auto=info.detection.board_auto=true;
  await page.reload();await page.waitForFunction(()=>!document.getElementById('choices').disabled);
  assert.equal(await page.inputValue('#variant'),'3');assert.equal(await page.inputValue('#microphone'),'3');
  assert(!await dirty('variant'));assert(!await dirty('microphone'));assert.deepEqual(errors,[]);
  results.push({packed,width,theme,style});await page.close();
 }
 fs.writeFileSync('tmp/qualification/auto-ui/setup-results.json',JSON.stringify(results,null,2));console.log('Setup Auto/manual: migration display, source/packed, mobile/desktop, light/dark, typography, drafts/poll/revert/discard/error/save and no-op passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
