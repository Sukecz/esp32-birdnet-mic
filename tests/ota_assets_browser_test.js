// Run after test_web_assets.py renders the actual C++ handler into tmp/.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {chromium}=require('playwright-core');
const http=require('node:http');
const pages=JSON.parse(fs.readFileSync('tmp/ota-rendered-pages.json','utf8'));
const header=fs.readFileSync('esp32-birdnet-mic/OtaAssets_gz.h','utf8');
function gzip(symbol){return Buffer.from([...header.match(new RegExp(symbol+'\\[\\].*?=\\s*\\{([\\s\\S]*?)\\};'))[1].matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)));}
(async()=>{
 let action,requests=[];
 const server=http.createServer((request,response)=>{
  const url=new URL(request.url,'http://localhost');
  if(url.pathname==='/ota.css'||url.pathname==='/ota.js'){
   const body=gzip(url.pathname.endsWith('.css')?'OTA_CSS_GZ':'OTA_JS_GZ');
   response.writeHead(200,{'Content-Type':url.pathname.endsWith('.css')?'text/css':'application/javascript','Content-Encoding':'gzip','Content-Length':body.length});
   response.end(body);return;
  }
  if(url.pathname==='/ota'){response.writeHead(200,{'Content-Type':'text/html'});response.end(pages[action==='checking'?0:2]);return;}
  const body=[];request.on('data',part=>body.push(part));request.on('end',()=>{
   requests.push({path:url.pathname,method:request.method,headers:request.headers,body:Buffer.concat(body)});
   if(url.pathname==='/api/update_space'){response.writeHead(200,{'Content-Type':'application/json'});response.end('{"state":"ready","capacity":2031616,"ota_allowed":true,"ota_message":"","message":""}');return;}
   if(url.pathname==='/api/ota_status'){response.writeHead(200,{'Content-Type':'application/json'});response.end('{"checking":true}');return;}
   response.writeHead(200,{'X-ESP32MIC-OTA-Reboot':'1'});response.end('installed');
  });
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const origin='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM||chromium.executablePath(),headless:true,args:['--no-sandbox']});
 try{
  for(action of ['install','upload','checking']){
   requests=[];
   const page=await browser.newPage(),errors=[];
   page.on('pageerror',error=>errors.push(error.message));
   page.on('dialog',dialog=>dialog.accept());
   await page.goto(origin+'/ota');
   assert.equal(await page.locator('#ota-progress').isVisible(),false);
   assert.equal(await page.locator('.card').first().evaluate(el=>getComputedStyle(el).borderRadius),'16px');
   if(action==='checking'){
    await page.waitForFunction(()=>document.body.dataset.otaChecking==='1');
    await new Promise(resolve=>setTimeout(resolve,850));
    assert(requests.some(r=>r.path==='/api/ota_status'));
   }else{
    if(action==='upload')await page.locator('input[type=file]').setInputFiles({name:'firmware-app-c6.bin',mimeType:'application/octet-stream',buffer:Buffer.from('mock firmware')});
    await page.locator(action==='install'?'#ota-install-form button':'#ota-upload-form button').click();
    await page.waitForFunction(()=>document.getElementById('ota-progress-title').textContent==='Firmware installed');
    assert.equal(await page.locator('#ota-progress').isVisible(),true);
    const sent=requests.find(r=>r.path==='/ota/'+action);
    assert(sent);assert.equal(sent.method,'POST');assert.equal(sent.headers['x-esp32mic-csrf'],'1');
    if(action==='upload')assert(sent.body.includes(Buffer.from('mock firmware')));
    assert(await page.locator('button:disabled').count()>0);
   }
   assert.deepEqual(errors,[]);
   await page.close();
  }
  console.log('OTA browser: gzip CSS/JS loading, upload/install CSRF headers, progress overlay, reboot state and update-check polling passed');
 }finally{await browser.close();server.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
