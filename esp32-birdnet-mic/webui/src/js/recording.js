
let recordingState=null, recordingPending=false, recordingDirty=false;
let recordingLogState=null,recordingLogDirty=false,recordingLogPending=false;
function recordingLogValues(){return {sd_enabled:recordingNode('recording_log_sd').checked?'1':'0',detailed:recordingNode('recording_log_level').value,max_kib:String(Number(recordingNode('recording_log_max').value)===Number((recordingLogState.max_kib*1024/1e6).toFixed(2))?recordingLogState.max_kib:(Number(recordingNode('recording_log_max').value)===16.78?16384:Math.round(Number(recordingNode('recording_log_max').value)*1e6/1024)))};}
function recordingLogDraftChanged(){
 if(!recordingLogState)return;
 const v=recordingLogValues();recordingLogDirty=Number(v.sd_enabled)!==Number(recordingLogState.sd_enabled)||Number(v.detailed)!==Number(recordingLogState.detailed)||Number(v.max_kib)!==recordingLogState.max_kib;
 recordingNode('recording_log_dirty').textContent=recordingLogDirty?'Unsaved':'';
 const busy=recordingLogPending||recordingPending||!!(recordingState&&(recordingState.busy||recordingState.cooling_pending));
 recordingNode('recording_log_save').disabled=busy||!recordingLogDirty;
}
function renderRecordingLog(r){
 if(!r.logging)return;recordingLogState=r.logging;
 if(!recordingLogDirty){recordingNode('recording_log_sd').checked=r.logging.sd_enabled;recordingNode('recording_log_level').value=r.logging.detailed?'1':'0';recordingNode('recording_log_max').value=(r.logging.max_kib*1024/1e6).toFixed(2);}
 const busy=r.busy||r.cooling_pending||recordingLogPending||recordingPending;
 for(const id of ['recording_log_sd','recording_log_level','recording_log_max'])recordingNode(id).disabled=busy;
 recordingNode('recording_log_status').textContent=r.logging.error||(!r.logging.sd_enabled?'SD logging disabled.':'SD log: '+r.logging.file+' · '+friendlyBytes(r.logging.bytes)+' · limit '+friendlyBytes(r.logging.max_kib*1024)+' per file.');
 recordingLogDraftChanged();
}
async function saveRecordingLogSettings(){
 const v=recordingLogValues();const n=Number(v.max_kib);
 if(!Number.isInteger(n)||n<64||n>16384){recordingNode('recording_log_feedback').textContent='Choose a log limit from 0.07 to 16.78 MB.';return;}
 if(await recordingPost('/api/recording/log/settings',v)){recordingLogDirty=false;recordingNode('recording_log_feedback').textContent='Log settings saved.';}
 await loadStatus();
}
let fieldPowerState=null,fieldPowerDirty=false,fieldPowerPending=false;
const fieldPowerIds=['field_wifi_delay','field_guard','field_cutoff','field_confirm','field_hysteresis','field_sleep'];
function fieldPowerDraft(){
 return {wifi_delay_seconds:Number(recordingNode('field_wifi_delay').value),enabled:recordingNode('field_guard').checked?1:0,cutoff_mv:Math.round(Number(recordingNode('field_cutoff').value)*1000),confirm_seconds:Number(recordingNode('field_confirm').value),hysteresis_mv:Math.round(Number(recordingNode('field_hysteresis').value)*1000),sleep_after_save:Number(recordingNode('field_sleep').value)};
}
function fieldPowerDraftChanged(){
 if(!fieldPowerState)return;
 recordingDraftChanged();
 const draft=fieldPowerDraft();
 fieldPowerDirty=Object.keys(draft).some(k=>Number(fieldPowerState[k])!==draft[k]);
 recordingNode('field_power_dirty').textContent=fieldPowerDirty?'Unsaved':'';
 const busy=fieldPowerPending||recordingPending||!!(recordingState&&(recordingState.busy||recordingState.cooling_pending));
 recordingNode('field_wifi_save').disabled=busy||draft.wifi_delay_seconds===Number(fieldPowerState.wifi_delay_seconds);
 recordingNode('field_power_save').disabled=busy||!Object.keys(draft).some(k=>k!=='wifi_delay_seconds'&&Number(fieldPowerState[k])!==draft[k]);
 recordingNode('recording_wifi_off').disabled=fieldPowerDirty||recordingDirty||fieldPowerPending||recordingPending||!recordingState||recordingState.state!=='recording';
 if(fieldPowerDirty)recordingNode('recording_start').disabled=true;
}
function renderFieldPower(j){
 if(wifiOffIntent||!j.field_power)return;
 const f=j.field_power;fieldPowerState=f;
 if(!fieldPowerDirty){
  recordingNode('field_wifi_delay').value=f.wifi_delay_seconds;
  recordingNode('field_guard').checked=f.enabled;
  recordingNode('field_cutoff').value=(f.cutoff_mv/1000).toFixed(2);
  recordingNode('field_confirm').value=f.confirm_seconds;
  recordingNode('field_hysteresis').value=(f.hysteresis_mv/1000).toFixed(2);
  recordingNode('field_sleep').value=f.sleep_after_save?'1':'0';
 }
 const busy=fieldPowerPending||recordingPending||!!(recordingState&&(recordingState.busy||recordingState.cooling_pending));
 for(const id of fieldPowerIds)recordingNode(id).disabled=busy;
 const states={disabled:'Protection disabled.',ready:'Protection ready.',measurement_unavailable:'Waiting for a valid battery reading. Recording cannot start while protection has no reading.',low_pending:'Low battery — confirming for '+f.remaining_seconds+' s.',saving:'Low battery — stopping and saving…',saved:'Recording stopped and saved because the battery was low.',save_failed:'Low battery stop: an SD error occurred. Check the preserved file.'};
 recordingNode('field_power_status').textContent=states[f.state]||f.state;let reason=recordingNode('recording_start_reason');if(!reason){reason=document.createElement('p');reason.id='recording_start_reason';reason.className='feedback';recordingNode('recording_start').parentElement.after(reason);}reason.textContent=f.start_allowed===false?(states[f.state]||'Battery protection prevents recording. Check Battery settings.'):'';
 const ack=recordingNode('field_acknowledge');ack.hidden=!f.latched;ack.disabled=!f.can_acknowledge||busy;
 if(j.wifi_off_pending)recordingNode('field_wifi_feedback').textContent='Wi-Fi switch-off is pending. Restart the device to reconnect.';
 else if(j.wifi_off_error)recordingNode('field_wifi_feedback').textContent=j.wifi_off_error+' Recording continues; you can retry.';
 fieldPowerDraftChanged();
}
async function saveFieldPowerSettings(wifiOnly=false){
 if(!fieldPowerState||fieldPowerPending||wifiOffIntent)return;
 const v=fieldPowerDraft(),feedback=recordingNode(wifiOnly?'field_wifi_feedback':'field_power_feedback');
 for(const k of Object.keys(v))if((k==='wifi_delay_seconds')!==wifiOnly)v[k]=Number(fieldPowerState[k]);
 const ids=fieldPowerIds.filter(id=>wifiOnly?id==='field_wifi_delay':id!=='field_wifi_delay');if(ids.some(id=>{const e=recordingNode(id);return e.type==='number'&&(!e.value.trim()||!e.checkValidity());})){feedback.textContent='Enter a valid value in each field.';return;}
 if(!Object.values(v).every(Number.isInteger)||v.wifi_delay_seconds<0||v.wifi_delay_seconds>60||v.cutoff_mv<3000||v.cutoff_mv>4200||v.confirm_seconds<0||v.confirm_seconds>300||v.hysteresis_mv<0||v.hysteresis_mv>300){feedback.textContent='Check the voltage and time ranges.';return;}
 fieldPowerPending=true;renderFieldPower({field_power:fieldPowerState});feedback.textContent='Saving…';
 try{
  const response=await fetch('/api/field_power',{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8','X-ESP32MIC-CSRF':'1'},body:new URLSearchParams(wifiOnly?{wifi_delay_seconds:v.wifi_delay_seconds}:Object.fromEntries(Object.entries(v).filter(([k])=>k!=='wifi_delay_seconds'))),signal:AbortSignal.timeout(15000)});
  const result=await response.json();if(!response.ok||!result.ok)throw new Error(result.error||'Could not save');
  feedback.textContent='Power settings saved.';await loadStatus();
 }catch(e){feedback.textContent=e.message;}
 finally{fieldPowerPending=false;renderFieldPower({field_power:fieldPowerState});}
}
async function switchRecordingWifiOff(){
 if(!fieldPowerState||!recordingState||recordingState.state!=='recording'||fieldPowerDirty||recordingDirty)return;
 const seconds=fieldPowerState.wifi_delay_seconds;
 if(!confirm('Turn Wi-Fi off in '+seconds+' seconds? Recording continues, then the device sleeps when it finishes. Restart the device to reconnect.'))return;
 if(!await recordingPost('/api/recording/action',{action:'wifi_off'}))return;
 wifiOffIntent=true;
 const message='Turning Wi-Fi off in '+seconds+' s. Recording continues; the device sleeps when recording ends.';
 recordingNode('field_wifi_feedback').textContent=message;
 recordingNode('connection_status').textContent=message;
 const controls=[...document.querySelectorAll('button,input,select')].map(el=>({el,disabled:el.disabled}));
 controls.forEach(({el})=>el.disabled=true);
 if(typeof stopListening==='function')stopListening('Wi-Fi switch-off requested.');
 await monitorRecordingWifiOff(seconds,controls);
}

async function monitorRecordingWifiOff(seconds,controls){
 const began=performance.now(),delay=seconds*1000,deadline=delay+12000;
 let failures=0,error='Wi-Fi is still available. Switch-off was not confirmed; try again.';
 while(performance.now()-began<deadline){
  await new Promise(resolve=>setTimeout(resolve,500));
  try{
   const response=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(2000)});
   if(!response.ok)throw new Error('Status unavailable');
   const status=await response.json();failures=0;
   if(status.wifi_off_error){error=status.wifi_off_error+' Recording continues; you can retry.';break;}
   if(!status.wifi_off_pending&&!status.wifi_suspended&&performance.now()-began>delay+1000)break;
  }catch(e){
   // Loss of contact is expected only after the switch-off delay. A transient
   // failed request while the radio is still on must not freeze the UI.
   if(performance.now()-began>=delay&&++failures>=2){
    const message='Control connection closed after the Wi-Fi-off request. Recording continues independently; the device sleeps when it ends. Restart the device to reconnect. Displayed values are now frozen.';
    recordingNode('field_wifi_feedback').textContent=message;
    recordingNode('connection_status').textContent=message;
    setTimeout(()=>checkWifiOffRecovery(controls),15000);
    return;
   }
  }
 }
 wifiOffIntent=false;
 controls.forEach(({el,disabled})=>el.disabled=disabled);
 await loadAll(true);
 recordingNode('field_wifi_feedback').textContent=error;
 recordingNode('connection_status').textContent=error;
 fieldPowerDraftChanged();
}

async function checkWifiOffRecovery(controls){
 if(!wifiOffIntent)return;
 try{
  const response=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(2000)});
  if(response.ok){
   const status=await response.json();
   if(!status.wifi_off_pending&&!status.wifi_suspended){
    wifiOffIntent=false;controls.forEach(({el,disabled})=>el.disabled=disabled);
    await loadAll(true);fieldPowerDraftChanged();
    recordingNode('field_wifi_feedback').textContent=status.wifi_off_error||'Device is reachable again. Controls restored.';
    return;
   }
  }
 }catch(e){}
 setTimeout(()=>checkWifiOffRecovery(controls),15000);
}

const recordingNode=id=>document.getElementById(id);
function recordingDurationModeChanged(){const limited=recordingNode('recording_duration_mode').value==='1';recordingNode('recording_hours').value=limited?(Number(recordingNode('recording_hours').value)||60):0;recordingNode('recording_hours').hidden=!limited;recordingNode('recording_minutes_label').hidden=!limited;recordingDraftChanged();}
function recordingDraftChanged(){
 if(!recordingState)return;
 recordingDirty=!recordingNode('recording_hours').value.trim()||recordingNode('recording_duration_mode').value!==(recordingState.duration_minutes?'1':'0')||Math.round(Number(recordingNode('recording_hours').value))!==recordingState.duration_minutes||Number(recordingNode('recording_segment').value)!==recordingState.segment_minutes||recordingNode('recording_auto').checked!==recordingState.auto_start;
 recordingNode('recording_dirty').textContent=recordingDirty?'Unsaved':'';
 recordingNode('recording_save').disabled=!recordingDirty||recordingPending||recordingState.busy||recordingState.cooling_pending;
 recordingNode('recording_start').disabled=recordingDirty||recordingPending||fieldPowerPending||recordingState.busy||recordingState.cooling_pending||!recordingState.supported||(fieldPowerState&&fieldPowerState.start_allowed===false);
}
function recordingTime(seconds){if(seconds>=86400)return Math.floor(seconds/86400)+' days '+Math.floor(seconds%86400/3600)+' hours';if(seconds>=3600)return Math.floor(seconds/3600)+'h '+Math.floor(seconds%3600/60)+'m';return Math.floor(seconds/60)+'m '+Math.floor(seconds%60)+'s';}
function renderRecording(j){
 if(wifiOffIntent)return;
 const r=j.recording;if(!r)return;
 document.documentElement.dataset.deviceMode=r.mode;
 const healthTitle=recordingNode('t_health');
 if(healthTitle)healthTitle.textContent=r.mode==='recording'?'Microphone health':'Stream health';
 const first=!recordingState;recordingState=r;
 const storage=recordingNode('recording_storage');
 if(storage)storage.textContent=r.storage==='sense_builtin'?'Sense built-in microSD slot · FAT32, up to 32 GB. Fit the Sense expansion board with its SD connection enabled.':r.storage==='unavailable'?'This board has no supported SD interface.':'External SPI microSD module.';
 recordingNode('recording_panel').hidden=r.mode!=='recording';
 recordingNode('device_mode').value=r.mode;
 recordingNode('device_mode').disabled=recordingPending;
 let text='Ready',color='muted',label=r.mode==='recording'?'Recording':'Streaming';
 if(r.mode==='recording'){
   const names={ready:r.card_ready?'Ready':'SD not checked',checking:'Checking SD…',starting:'Preparing SD…',recording:'OK',saving:'Saving…',saved:'Saved',error:'Error',cooling:'Paused — cooling down'};
   text=names[r.state]||r.state;
   color=r.state==='error'||!r.supported?'bad':r.state==='recording'||r.state==='ready'&&r.card_ready?'ok':'muted';
   if(r.cooling_pending)color='warn';
   if(!r.supported)text='Error — SD recording unavailable for this board';
   else if(r.state==='error'&&r.error)text+=' — '+(r.error.startsWith('SD unavailable')?'SD unavailable':r.error);
 }else if(j.s1_streaming||j.s2_streaming){text='OK';color='ok';}
 else if(!j.rtsp_server_enabled){text=j.stream_schedule_enabled&&!j.stream_schedule_allow_now?'Paused by schedule':'Stopped';}
 const status=recordingNode('device_mode_status');
 status.textContent=label+': '+text;status.className=color;
 status.href=r.mode==='recording'?'#recording_panel':'#streamssec';
 status.onclick=()=>{if(r.mode!=='recording')openSection('streamssec');};
 const remaining=r.duration_minutes?'; remaining '+recordingTime(Math.max(0,r.duration_minutes*60-r.elapsed_seconds)):'';
 recordingNode('recording_summary').textContent=(r.supported?(r.error||text):'SD recording is unavailable for this board.')+' · '+recordingTime(r.elapsed_seconds)+remaining+' · '+friendlyBytes(r.written_kib*1024)+' saved · '+(r.card_ready?friendlyBytes(r.free_mib*1048576)+' free':'Card not checked')+(r.dropped_samples?' · '+r.dropped_samples+' samples lost':'');
 const capacity=recordingNode('recording_capacity');
 if(capacity){
  const seconds=r.space_seconds;
  const insufficient=r.duration_minutes>0&&Number.isFinite(seconds)&&seconds<Math.max(0,r.duration_minutes*60-r.elapsed_seconds);
  capacity.textContent=Number.isFinite(seconds)?'Space for approximately '+recordingTime(seconds)+' of audio'+(insufficient?' — less than the remaining recording duration.':'.'):'Recording time estimate unavailable until the SD card is checked.';
  capacity.className='section-note'+(insufficient?' warn':'');
 }
 recordingNode('recording_file').textContent=r.file||'';
 if(first||!recordingDirty){recordingNode('recording_hours').value=r.duration_minutes;recordingNode('recording_duration_mode').value=r.duration_minutes?'1':'0';recordingNode('recording_hours').hidden=!r.duration_minutes;recordingNode('recording_minutes_label').hidden=!r.duration_minutes;recordingNode('recording_segment').value=String(r.segment_minutes);recordingNode('recording_auto').checked=r.auto_start;}
 for(const id of ['recording_duration_mode','recording_hours','recording_segment','recording_auto'])recordingNode(id).disabled=r.busy||r.cooling_pending||recordingPending;
 recordingNode('recording_stop').disabled=(!r.busy&&!r.cooling_pending)||recordingPending;
 const check=recordingNode('recording_check');if(check)check.disabled=r.busy||r.cooling_pending||recordingPending||!r.supported;
 recordingDraftChanged();
 if(fieldPowerState)renderFieldPower({...j,field_power:fieldPowerState});
 renderRecordingLog(r);
}
async function recordingPost(path,values){
 if(recordingPending||wifiOffIntent)return false;
 recordingPending=true;if(recordingState)renderRecording({recording:recordingState,field_power:fieldPowerState});
 recordingNode('recording_feedback').textContent='Working…';
 recordingNode('device_mode_feedback').textContent='';
 try{
  const response=await fetch(path,{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8','X-ESP32MIC-CSRF':'1'},body:new URLSearchParams(values),signal:AbortSignal.timeout(15000)});
  const result=await response.json();
  if(!response.ok||!result.ok)throw new Error(result.error||'Action failed');
  recordingNode('recording_feedback').textContent='';return true;
 }catch(e){recordingNode('recording_feedback').textContent=e.message;recordingNode('device_mode_feedback').textContent=e.message;return false;}
 finally{recordingPending=false;}
}
async function changeDeviceMode(mode){
 const current=recordingState;
 if(current&&current.busy&&!confirm('Stop and save the recording before switching mode?')){recordingNode('device_mode').value=current.mode;return;}
 await recordingPost('/api/device/mode',{mode,stop_confirmed:current&&current.busy?'1':'0'});
 await loadStatus();
}
async function recordingAction(action){
 await recordingPost('/api/recording/action',{action});await loadStatus();
}
async function saveRecordingSettings(){
 if(!recordingNode('recording_hours').value.trim()){recordingNode('recording_feedback').textContent='Enter a duration, or 0 to record until stopped.';return;}
 const rawMinutes=Number(recordingNode('recording_hours').value);
 const minutes=Math.abs(rawMinutes-Math.round(rawMinutes))<1e-8?Math.round(rawMinutes):rawMinutes;
 if(!Number.isInteger(minutes)||minutes<0||minutes>10080||(recordingNode('recording_duration_mode').value==='1'&&minutes===0)){recordingNode('recording_feedback').textContent='Choose a duration from 0 to 10080 minutes.';return;}
 if(await recordingPost('/api/recording/settings',{duration_minutes:String(minutes),segment_minutes:recordingNode('recording_segment').value,auto_start:recordingNode('recording_auto').checked?'1':'0'}))recordingDirty=false;
 await loadStatus();
}
