const fs=require('node:fs'),zlib=require('node:zlib');
exports.apPassword=function(route,packed=false){
 let body=fs.readFileSync('esp32-birdnet-mic/webui/ap-password.js','utf8');
 if(packed){
  const header=fs.readFileSync('esp32-birdnet-mic/SetupAssets_gz.h','utf8'),start=header.indexOf('AP_PASSWORD_JS_GZ[]');
  body=zlib.gunzipSync(Buffer.from([...header.slice(start,header.indexOf('};',start)).matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
 }
 return route.fulfill({contentType:'application/javascript',body});
};
