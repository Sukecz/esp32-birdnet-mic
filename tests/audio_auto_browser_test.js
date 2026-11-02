const assert=require('node:assert/strict'),fs=require('node:fs');
const {chromium}=require('playwright-core');
const assets=require('./ui_assets');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 fs.mkdirSync('tmp/qualification/auto-ui',{recursive:true});
 const results=[];
 try{for(const packed of [false,true])for(const width of [320,390,1280])for(const theme of ['light','dark']){
  const fixture=JSON.parse(fs.readFileSync('tests/fixtures/ui-state.json'));
  fixture.audio_status.detection={mic_auto:false,board_auto:false,note:'Manual board and microphone settings retained.'};
  fixture.audio_status.s3_board_supported=true;fixture.audio_status.s3_board=0;
  fixture.audio_status.noise={mode:0,floor_db:12,adapt_s:15,active:false,learn_pct:0,cpu_pct:0,cpu_peak_pct:0,est_reduction_db:0};
  const writes=[],errors=[];let fail=false;
  const page=await browser.newPage({viewport:{width,height:1000}});page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://auto.test/**',route=>{
   const path=new URL(route.request().url()).pathname;
   if(path==='/')return route.fulfill({contentType:'text/html',body:assets.main(packed)});
   if(path==='/ap-password.js')return assets.apPassword(route,packed);
   if(route.request().method()==='POST'){
    const v=Object.fromEntries(new URLSearchParams(route.request().postData()));writes.push({path,v});
    if(fail)return route.fulfill({status:500,json:{ok:false,error:'test failure'}});
    if(path==='/api/noise/set')Object.assign(fixture.audio_status.noise,{mode:Number(v.mode),floor_db:Number(v.floor),adapt_s:Number(v.adapt)});
    if(path==='/api/settings/s3_board')fixture.audio_status.detection.board_auto=v.variant==='3';
    if(path==='/api/settings/microphone')fixture.audio_status.detection.mic_auto=v.format==='3';
    return route.fulfill({json:{ok:true}});
   }
   if(path==='/api/noise')return route.fulfill({json:fixture.audio_status.noise});
   if(path==='/api/logs')return route.fulfill({body:'test'});
   return route.fulfill({json:fixture[path.slice(5)]||{ok:true,available:false}});
  });
  await page.goto('http://auto.test/');await page.waitForFunction(()=>document.getElementById('in_gain').value==='1.50');
  await page.selectOption('#themeSel',theme);await page.evaluate(()=>openSection('audiosec'));
  const dirty=async id=>page.locator('#'+id).evaluate(e=>e.classList.contains('dirty'));
  for(const [id,changed,original] of [['sel_s3_board','3','0'],['sel_mic_format','3','0'],['noise_mode','1','0']]){
   await page.selectOption('#'+id,changed);assert(await dirty(id));
   await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#'+id),changed);
   await page.selectOption('#'+id,original);assert(!await dirty(id));
  }
  assert.equal(writes.length,0);
  await page.locator('#noise_details').evaluate(e=>e.open=true);
  for(const [id,changed,original] of [['noise_floor','6','12'],['noise_adapt','60','15']]){
   await page.selectOption('#'+id,changed);assert(await dirty(id));
   await page.evaluate(()=>refreshAll());assert.equal(await page.inputValue('#'+id),changed);
   await page.selectOption('#'+id,original);assert(!await dirty(id));
  }
  await page.selectOption('#noise_mode','1');await page.selectOption('#noise_floor','6');
  await page.fill('#in_gain','2.25');fail=true;await page.click('#noise_save');
  await page.waitForFunction(()=>!sectionBusy.has('audiosec'));assert(await dirty('noise_mode'));assert(await dirty('noise_floor'));
  assert.match(await page.locator('#audiosec>.feedback').textContent(),/kept/);
  fail=false;await page.click('#noise_save');await page.waitForFunction(()=>!sectionBusy.has('audiosec'));
  assert(!await dirty('noise_mode'));assert(!await dirty('noise_floor'));assert(await dirty('in_gain'));
  assert.deepEqual(Object.keys(writes.at(-1).v).sort(),['adapt','floor','mode']);
  await page.selectOption('#noise_adapt','60');await page.locator('#audiosec>.discard').click();
  assert.equal(await page.inputValue('#noise_adapt'),'15');assert(!await dirty('noise_adapt'));
  // Saved board and mic choices are independent, and saving one preserves other drafts.
  await page.selectOption('#sel_mic_format','3');await page.selectOption('#sel_s3_board','3');
  await page.click('#btn_s3_board_apply');await page.waitForFunction(()=>!sectionBusy.has('audiosec'));
  assert(await dirty('sel_mic_format'));assert(!await dirty('sel_s3_board'));
  await page.click('#btn_mic_apply');await page.waitForFunction(()=>!sectionBusy.has('audiosec'));
  assert(!await dirty('sel_mic_format'));
  let count=writes.length;await page.evaluate(()=>{saveS3Board();chooseMicSettings();saveNoiseSettings();});assert.equal(writes.length,count);
  await page.selectOption('#noise_adapt','60');
  const styles=await page.locator('#noise_adapt').evaluate(e=>({border:getComputedStyle(e).borderColor,font:getComputedStyle(e).fontSize,reference:getComputedStyle(document.getElementById('sel_hp')).fontSize}));
  assert.equal(styles.font,styles.reference);assert.match(styles.border,/rgb\((217, 119, 6|245, 158, 11)\)/);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  if(packed)await page.locator('#audiosec').screenshot({path:`tmp/qualification/auto-ui/audio-${width}-${theme}.png`});
  assert.deepEqual(errors,[]);results.push({packed,width,theme,styles});await page.close();
 }
 fs.writeFileSync('tmp/qualification/auto-ui/results.json',JSON.stringify(results,null,2));
 console.log('Auto board/mic + noise: source/embedded, 320/390/1280, light/dark, typography, drafts, polling, revert/save/error/discard and scopes passed');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
