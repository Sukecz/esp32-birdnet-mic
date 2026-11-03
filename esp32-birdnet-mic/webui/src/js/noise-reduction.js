const noiseFields=[['noise_mode','noise_mode','mode','noise_save','mode'],['noise_floor','noise_floor','floor_db','noise_floor_apply','floor'],['noise_adapt','noise_adapt','adapt_s','noise_adapt_apply','adapt']];
let noiseSettingsLoaded=false;
for(const [id,key] of noiseFields){trackEdit($(id),key);$(id).addEventListener('change',updateNoiseSave);}
function updateNoiseSave(){
 const busy=!!overviewState.status?.recording?.busy;$('noise_busy_hint').hidden=!busy;
 for(const [,key,,button] of noiseFields)$(button).disabled=!noiseSettingsLoaded||busy||sectionBusy.has('noisesec')||!differsFromSaved(key);
}
function renderNoiseSettings(s){
 if(!s)return;
 noiseSettingsLoaded=true;
 for(const [id,key,field] of noiseFields)syncInput($(id),key,String(s[field]));
 const reason=s.error||'';
 const error=reason.includes('Insufficient memory')?'Not enough memory. Audio is unchanged.':reason.includes('CPU too slow')?'Cannot keep up. Audio is unchanged. Increase CPU speed or turn the filter off.':reason?'Could not start. Audio is unchanged. Turn Off and On to retry.':'';
 $('noise_status').textContent=error;$('noise_status').hidden=!error;
 $('noise_metrics').textContent=s.mode&&s.active&&!s.learned?'Learning background: '+s.learn_pct+'%':'';
 $('noise_metrics').hidden=!$('noise_metrics').textContent;
 updateNoiseSave();
}
async function saveNoiseSettings(key='noise_mode'){
 const row=noiseFields.find(([,field])=>field===key);
 if(!noiseSettingsLoaded||!row||!differsFromSaved(key))return;
 // The endpoint takes a full configuration. Only this row uses its draft;
 // other rows use their confirmed saved values, preserving unrelated edits.
 const values=Object.fromEntries(noiseFields.map(([,field,,,param])=>[param,confirmedValues[field]]));
 values[row[4]]=$(row[0]).value;
 await saveValues('/api/noise/set',values,[key],'noisesec');updateNoiseSave();await updateNoiseSpectrum();
}
let noiseGraphBusy=false;
async function updateNoiseSpectrum(){
 if(!$('noisesec').open||noiseGraphBusy)return;
 noiseGraphBusy=true;
 try{
  // Spectrum requests never replace saved settings from the main status poll.
  const s=await fetchJSON('/api/noise?spectra=1');
  const canvas=$('noise_spectrum'),ctx=canvas.getContext('2d'),w=canvas.clientWidth,h=240,dpr=window.devicePixelRatio||1;
  canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,w,h);ctx.font=getComputedStyle(canvas).font;ctx.fillStyle=getComputedStyle(document.body).color;
  if(!s.active||!s.level){ctx.fillText(s.mode?(s.error?'Graph unavailable.':'Waiting for audio…'):'Turn noise reduction on',12,30);if(!s.mode)ctx.fillText('to view the graph.',12,52);return;}
  const db=q=>(q-14514.409)*3.0103/256;
  const x=k=>48+k*(w-64)/256,y=q=>Math.max(12,Math.min(h-30,12+(-db(q))*(h-42)/120));
  ctx.strokeStyle=getComputedStyle(canvas).borderColor;ctx.lineWidth=1;
  for(const level of [0,-30,-60,-90,-120]){const yy=12-level*(h-42)/120;ctx.fillText(String(level),4,yy+5);ctx.beginPath();ctx.moveTo(48,yy);ctx.lineTo(w-16,yy);ctx.stroke();}
  for(const k of [0,64,128,192,256])ctx.fillText((k*s.sample_rate/512000).toFixed(0),x(k)-6,h-8);
  for(const [key,color] of [['level','#3b82f6'],['floor','#f59e0b'],['signature','#10b981']]){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();s[key].forEach((q,k)=>k?ctx.lineTo(x(k),y(q)):ctx.moveTo(x(k),y(q)));ctx.stroke();}
 }catch(_){$('noise_metrics').textContent='Could not load the graph. Retrying…';$('noise_metrics').hidden=false;}finally{noiseGraphBusy=false;}
}
$('noisesec').addEventListener('toggle',updateNoiseSpectrum);
setInterval(updateNoiseSpectrum,3000);
