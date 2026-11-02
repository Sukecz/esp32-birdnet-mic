const noiseFields=[['noise_mode','noise_mode','mode'],['noise_floor','noise_floor','floor_db'],['noise_adapt','noise_adapt','adapt_s']];
for(const [id,key] of noiseFields){trackEdit($(id),key);$(id).addEventListener('change',updateNoiseSave);}
function updateNoiseSave(){const busy=!!overviewState.status?.recording?.busy;$('noise_save').disabled=busy||sectionBusy.has('audiosec')||!noiseFields.some(([,key])=>differsFromSaved(key));}
function renderNoiseSettings(s){
 if(!s)return;
 for(const [id,key,field] of noiseFields)syncInput($(id),key,String(s[field]));
 $('noise_status').textContent=s.error||(s.mode?(s.active?(s.learned?'Running':'Learning noise profile: '+s.learn_pct+'%'):'Starting…'):'Off — original audio');
 $('noise_status').className='section-note '+(s.error?'warn':'');
 $('noise_metrics').textContent=s.active?`Processing: ${s.cpu_pct}% of audio time · Peak: ${s.cpu_peak_pct}% · Estimated reduction: ${s.est_reduction_db} dB · Working memory: ${s.engine_psram?'PSRAM':'internal RAM'}`:'';
 updateNoiseSave();
}
async function saveNoiseSettings(){
 if(!noiseFields.some(([,key])=>differsFromSaved(key)))return;
 await saveValues('/api/noise/set',{mode:$('noise_mode').value,floor:$('noise_floor').value,adapt:$('noise_adapt').value},noiseFields.map(([,key])=>key),'audiosec','Noise reduction settings saved.');updateNoiseSave();
}
let noiseGraphBusy=false;
async function updateNoiseSpectrum(){
 if(!$('noise_details').open||!$('audiosec').open||noiseGraphBusy)return;
 noiseGraphBusy=true;
 try{
  const s=await fetchJSON('/api/noise?spectra=1');renderNoiseSettings(s);
  const canvas=$('noise_spectrum'),ctx=canvas.getContext('2d'),w=canvas.clientWidth,h=240,dpr=window.devicePixelRatio||1;
  canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,w,h);ctx.font=getComputedStyle(canvas).font;ctx.fillStyle=getComputedStyle(document.body).color;
  if(!s.active||!s.level){ctx.fillText('Enable noise reduction',12,30);ctx.fillText('to view its spectrum.',12,52);return;}
  const db=q=>(q-14514.409)*3.0103/256;
  const x=k=>48+k*(w-64)/256,y=q=>Math.max(12,Math.min(h-30,12+(-db(q))*(h-42)/120));
  ctx.strokeStyle=getComputedStyle(canvas).borderColor;ctx.lineWidth=1;
  for(const level of [0,-30,-60,-90,-120]){const yy=12-level*(h-42)/120;ctx.fillText(String(level),4,yy+5);ctx.beginPath();ctx.moveTo(48,yy);ctx.lineTo(w-16,yy);ctx.stroke();}
  for(const k of [0,64,128,192,256])ctx.fillText((k*s.sample_rate/512000).toFixed(0),x(k)-6,h-8);
  for(const [key,color] of [['level','#3b82f6'],['floor','#f59e0b'],['signature','#10b981']]){ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();s[key].forEach((q,k)=>k?ctx.lineTo(x(k),y(q)):ctx.moveTo(x(k),y(q)));ctx.stroke();}
 }catch(_){$('noise_metrics').textContent='Spectrum unavailable. Retrying while details are open.';}finally{noiseGraphBusy=false;}
}
$('noise_details').addEventListener('toggle',updateNoiseSpectrum);
setInterval(updateNoiseSpectrum,3000);
