const T={en:{"title":"BirdNETMic","ip":"IP Address","wifi_rssi":"WiFi RSSI","uptime":"Uptime","local_time":"Device Local Time","utc_time":"UTC Time","time_unavailable":"Unavailable (unsynced)","net_reset_confirm":"Reset Wi-Fi credentials and reboot? You will need to reconfigure via the setup AP.","max_clients":"Max RTSP clients","audio":"Audio","gain":"Microphone gain","thermal":"Thermal & performance","restarting":"Restarting device...","resetting":"Restoring defaults and rebooting...","set":"Apply","adv_buf512":"If stream drops, try an RTP packet size of 512 or higher.","adv_buf1024":"For weak Wi-Fi on TCP clients, 1024 samples per RTP packet can help; use 512 for BirdNET-Pi UDP if 1024 stutters.","adv_gain":"High Gain can clip. Lower Gain or increase I2S Shift.","help_gain":"Amplifies audio after I2S shift; too high will clip.","therm_status_latched":"Cooling required - restart manually","therm_status_latched_persist":"Protection latched - acknowledge to re-enable","therm_last_none":"No shutdown recorded yet.","therm_last_fmt":"Stopped at %TEMP% C (limit %LIMIT% C) after %TIME% uptime (%AGO%).","therm_last_sensor_fault":"Thermal protection disabled: temperature sensor unavailable.","therm_time_unknown":"unknown time","therm_time_ago_unknown":"just now","clip_ok":"OK","clip_warn":"High level - close to clipping.","clip_bad":"CLIPPING! Reduce Gain or raise I2S Shift.","profile_ultra":"Ultra-Low Latency (Higher CPU, may have dropouts)","profile_balanced":"Balanced (Moderate CPU, good stability)","profile_stable":"Stable Streaming (Lower CPU, excellent stability)","profile_high":"High Stability (Lowest CPU, max stability)","yes":"YES","no":"NO"}};let lang='en'; let theme='auto';try{theme=localStorage.getItem('theme')||'auto';}catch(_){} const $=id=>document.getElementById(id);function applyTheme(){ const root=document.documentElement; if(theme==='light'||theme==='dark'){ root.setAttribute('data-theme', theme); } else { root.removeAttribute('data-theme'); theme='auto'; } const ts=$('themeSel'); if(ts) ts.value=theme; }const SCHED_TXT={en:{"state_off":"Schedule disabled","state_wait":"Time unavailable - fail-open (stream allowed)","state_allow":"Inside allowed window","state_block":"Outside allowed window","state_invalid":"Invalid window (start = stop) - stream blocked","ds_disabled":"Deep sleep disabled","ds_need_sched":"Enable Stream Schedule first","ds_invalid":"Invalid schedule window (start = stop) - deep sleep blocked","ds_need_time":"Time not synchronized - deep sleep blocked","ds_inside":"Inside stream window - device stays awake","ds_grace":"Startup grace period - waiting","ds_wait":"Outside window detected - waiting for stability","ds_client":"Client connected - sleep postponed","ds_streaming":"Streaming active - sleep postponed","ds_reboot":"Reboot pending - sleep postponed","ds_soon":"Next stream window is soon - no sleep","ds_ready":"Outside window - deep sleep armed"}};function minsToHHMM(mins){ mins=parseInt(mins,10); if(isNaN(mins)) mins=0; mins=((mins%1440)+1440)%1440; const h=Math.floor(mins/60), m=mins%60; return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0'); }function hhmmToMins(v){ if(!v || v.indexOf(':')<0) return null; const p=v.split(':'); if(p.length!==2) return null; const h=parseInt(p[0],10), m=parseInt(p[1],10); if(isNaN(h)||isNaN(m)||h<0||h>23||m<0||m>59) return null; return h*60+m; }function fmtSecondsBrief(sec){ sec=parseInt(sec,10); if(!isFinite(sec)||sec<0) sec=0; const h=Math.floor(sec/3600), m=Math.floor((sec%3600)/60), s=sec%60; if(h>0) return h+'h '+m+'m'; if(m>0) return m+'m '+s+'s'; return s+'s'; }

function deepSleepStatusHTML(code,nextSec,L){ let cls='muted', txt=L.ds_disabled||'Deep sleep disabled'; if(code==='schedule_disabled'){ cls='bad'; txt=L.ds_need_sched; } else if(code==='schedule_invalid'){ cls='bad'; txt=L.ds_invalid; } else if(code==='time_invalid'){ cls='warn'; txt=L.ds_need_time; } else if(code==='inside_window'){ cls='ok'; txt=L.ds_inside; } else if(code==='grace_boot'){ cls='muted'; txt=L.ds_grace; } else if(code==='outside_stabilizing'){ cls='muted'; txt=L.ds_wait; } else if(code==='client_connected'){ cls='warn'; txt=L.ds_client; } else if(code==='streaming_active'){ cls='warn'; txt=L.ds_streaming; } else if(code==='reboot_pending'){ cls='warn'; txt=L.ds_reboot; } else if(code==='next_window_soon'){ cls='muted'; txt=L.ds_soon; } else if(code==='ready'){ cls='warn'; txt=L.ds_ready; } if(nextSec>0){ txt += ' ('+fmtSecondsBrief(nextSec)+')'; } return '<span class='+cls+'>'+txt+'</span>'; }function profileText(buf){const L=T[lang]; buf=parseInt(buf,10)||0; if(buf<=256) return L.profile_ultra; if(buf<=512) return L.profile_balanced; if(buf<=1024) return L.profile_stable; return L.profile_high;}function fmtBool(b){const L=T[lang]; return b?`<span class=ok>${L.yes||'YES'}</span>`:`<span class=bad>${L.no||'NO'}</span>`}function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}function showOverlay(msg){ $('ovr_msg').textContent=msg; $('ovr').style.display='flex'; }function rebootSequence(kind){ const L=T[lang]; const msg=(kind==='factory_reset')?L.resetting:L.restarting; showOverlay(msg); function tick(){ fetch('/api/status',{cache:'no-store'}).then(r=>{ if(r.ok){ location.reload(); } else { setTimeout(tick,2000); } }).catch(()=>setTimeout(tick,2000)); } setTimeout(tick,4000); }function mutationFetch(url,body){return fetch(url,{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8','X-ESP32MIC-CSRF':'1'},body:body||'',signal:AbortSignal.timeout(10000)})}function act(a){
 const section=a.startsWith('mqtt')?'mqttsec':a==='time_sync'?'timesec':a.startsWith('server')?'streamssec':a.includes('wifi')||a==='network_reset'?'networksec':'recoverysec';
 return saveValues('/api/action/'+a,{},[],section,a==='time_sync'?'Synchronization requested.':'Done.');
}
async function rebootNow(){if(await act('reboot')) rebootSequence('reboot');}
async function defaultsNow(){
 if(!confirm('Restore factory settings and reboot? Audio, schedules, MQTT and access-point selection will be reset. The saved Wi-Fi network remains.'))return;
 if(await act('factory_reset')) rebootSequence('factory_reset');
}
/* @include "settings/groups.js" */const scheduleKeys=['stream_sched','stream_start_min','stream_stop_min','deep_sleep_sched'];
async function saveSchedule(){
 const start=hhmmToMins($('in_stream_start').value),stop=hhmmToMins($('in_stream_stop').value),enabled=$('sel_stream_sched_en').value,sleep=enabled==='on'?$('sel_deep_sleep_sched').value:'off';
 if(start===null||stop===null||(enabled==='on'&&start===stop)){feedback('timesec','Choose valid, different start and end times.','bad');return;}
 if(sleep==='on'&&!overviewState.status?.deep_sleep_sched_enabled&&!confirm('Enable sleep outside the streaming window? The device web page, audio and OTA will be unavailable while asleep. It wakes before the next window.'))return;
 await saveValues('/api/settings/schedule',{enabled,start,stop,sleep},scheduleKeys,'timesec','Schedule saved.');
}
const mqttConnectionKeys=['mqtt_enable','mqtt_host','mqtt_port','mqtt_user','mqtt_pass','mqtt_clear_password'];
async function saveMqttConnection(){
 const enabled=$('sel_mqtt_en').value,host=$('in_mqtt_host').value.trim(),port=$('in_mqtt_port').value;
 const password=$('in_mqtt_pass').value,clear=$('mqtt_clear_password').checked;
 if((enabled==='on'&&!host)||!$('in_mqtt_port').checkValidity()||!port||(clear&&password)){
  feedback('mqttsec','Enter a broker address and valid port. Choose either a new password or Clear.','bad');return;
 }
 if(await saveValues('/api/settings/mqtt',{enabled,host,port,user:$('in_mqtt_user').value,password,clear_password:clear?'on':'off'},mqttConnectionKeys,'mqttsec','Connection settings saved. Check connection status below.')){
  syncInput($('in_mqtt_pass'),'mqtt_pass','');
  syncInput($('mqtt_clear_password'),'mqtt_clear_password',false);
 }
}
function updateDependencies(){
 const schedule=$('sel_stream_sched_en').value==='on';
 $('sel_deep_sleep_sched').disabled=!schedule;
 $('schedule_draft_hint').textContent=schedule?'The streaming window uses device local time. Start and end are saved together.':'Schedule disabled. You can prepare a window, then enable and save it. Sleep will also be disabled.';
 if(typeof updateFeatureDependencies==='function')updateFeatureDependencies();
}
async function fetchJSON(url){const response=await fetch(url,{cache:'no-store',signal:AbortSignal.timeout(8000)});if(!response.ok)throw new Error('Device response unavailable');return response.json();}

const locks={}; const edits={}; const dirtyEls={};const LOG_STICKY_PX=24; let logAutoScroll=true;function isLogNearBottom(el){ return (el.scrollHeight - el.scrollTop - el.clientHeight) <= LOG_STICKY_PX; }function bindLogScroll(){ const lg=$('logs'); if(!lg || lg.dataset.bound==='1') return; lg.dataset.bound='1'; lg.addEventListener('scroll',()=>{ logAutoScroll=isLogNearBottom(lg); }); }function setv(k,v){
 v=String(v??'').trim().replace(',', '.');
 const els=dirtyEls[k]||[], section=els[0]?.closest('details.card')?.id;
 if(v===''||els.some(el=>!el.checkValidity())){feedback(section,'Enter a valid value within the displayed range.','bad');els.find(el=>!el.checkValidity())?.reportValidity();return Promise.resolve(false);}
 return saveValues('/api/set',{key:k,value:v},[k],section);
}
function setvRaw(k,v){return saveValues('/api/set',{key:k,value:String(v??'').trim()},[k],dirtyEls[k]?.[0]?.closest('details.card')?.id);}
function bindSaver(el,key){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){setv(key,el.value)}})}function bindEnter(el,fn){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){fn()}})}/* @include "settings/fields.js" */function setToggleState(on){const onb=$('b_srv_on'), offb=$('b_srv_off'); if(onb&&offb){onb.classList.toggle('active',on); offb.classList.toggle('active',!on); onb.disabled=on; offb.disabled=!on&&!thermalAutoPending;}}function renderTimeRows(j){
 const L=T[lang]||T.en, synced=!!j.time_synced;
 const valid=j.time_valid===undefined?synced:!!j.time_valid;
 const status=$('time_sync');
 if(status){
  const labels={disabled:'OFF',waiting_wifi:'Waiting for Wi-Fi',syncing:'Waiting for NTP',synced:L.yes||'YES'};
  const text=labels[j.time_sync_status]||(synced?(L.yes||'YES'):(L.no||'NO'));
  status.textContent=text;status.className='v '+(synced?'ok':'muted');
 }
 const syncNow=$('b_sync_now');if(syncNow)syncNow.disabled=!j.time_sync_enabled;
 const m=Number(j.effective_time_offset_min??j.time_offset_min??0);
 const abs=Math.abs(m), offset='UTC'+(m>=0?'+':'-')+String(Math.floor(abs/60)).padStart(2,'0')+':'+String(abs%60).padStart(2,'0');
 for(const [id,value,suffix] of [['local_time',j.local_time,' ('+offset+')'],['utc_time',j.utc_time,'']]){
  const el=$(id);if(!el)continue;
  const available=valid&&value&&value!=='unavailable';
  el.textContent=available?value+suffix:(L.time_unavailable||'Unavailable (unsynced)');
  el.className='v '+(available&&synced?'ok':'warn');
 }
}function loadStreamSchedule(j){ const now=Date.now(); const L=(SCHED_TXT[lang]||SCHED_TXT.en); const en=$('sel_stream_sched_en'); syncInput(en,'stream_sched',j.stream_schedule_enabled?'on':'off'); const st=$('in_stream_start'); syncInput(st,'stream_start_min',minsToHHMM(j.stream_schedule_start_min||0)); const sp=$('in_stream_stop'); syncInput(sp,'stream_stop_min',minsToHHMM(j.stream_schedule_stop_min||0)); const ds=$('sel_deep_sleep_sched'); syncInput(ds,'deep_sleep_sched',j.deep_sleep_sched_enabled?'on':'off'); const state=$('stream_sched_state'); if(state){ const start=(j.stream_schedule_start_min||0), stop=(j.stream_schedule_stop_min||0); const win=' ('+minsToHHMM(start)+'-'+minsToHHMM(stop)+')'; if(!j.stream_schedule_enabled){ state.innerHTML='<span class=muted>'+L.state_off+'</span>'; } else if(start===stop){ state.innerHTML='<span class=bad>'+L.state_invalid+win+'</span>'; } else if(!j.stream_schedule_time_valid){ state.innerHTML='<span class=warn>'+L.state_wait+win+'</span>'; } else if(j.stream_schedule_allow_now){ state.innerHTML='<span class=ok>'+L.state_allow+win+'</span>'; } else { state.innerHTML='<span class=bad>'+L.state_block+win+'</span>'; } } const dsState=$('deep_sleep_state'); if(dsState){ const code=String(j.deep_sleep_status_code||'disabled'); const next=parseInt(j.deep_sleep_next_sec||0,10)||0; dsState.innerHTML=deepSleepStatusHTML(code,next,L); } }



function setMqttTopic(){ const el=$('in_mqtt_topic'); if(!el) return; setvRaw('mqtt_topic',el.value); }function setMqttDiscovery(){ const el=$('in_mqtt_discovery'); if(!el) return; setvRaw('mqtt_discovery',el.value); }function setMqttClientId(){ const el=$('in_mqtt_client_id'); if(!el) return; setvRaw('mqtt_client_id',el.value); }function setMqttInterval(){ const el=$('in_mqtt_interval'); if(!el) return; setv('mqtt_interval',el.value); }function loadMqtt(j){ const now=Date.now(); const en=$('sel_mqtt_en'); syncInput(en,'mqtt_enable',j.mqtt_enabled?'on':'off'); const host=$('in_mqtt_host'); syncInput(host,'mqtt_host',j.mqtt_host||''); const port=$('in_mqtt_port'); syncInput(port,'mqtt_port',j.mqtt_port||1883); const user=$('in_mqtt_user'); syncInput(user,'mqtt_user',j.mqtt_user||''); const topic=$('in_mqtt_topic'); syncInput(topic,'mqtt_topic',j.mqtt_topic||''); const disc=$('in_mqtt_discovery'); syncInput(disc,'mqtt_discovery',j.mqtt_discovery||''); const cid=$('in_mqtt_client_id'); syncInput(cid,'mqtt_client_id',j.mqtt_client_id||''); const iv=$('in_mqtt_interval'); syncInput(iv,'mqtt_interval',j.mqtt_interval_sec||60); const conn=$('mqtt_conn'); if(conn){ if(!j.mqtt_enabled){ conn.innerHTML='<span class=muted>Disabled</span>'; } else if(j.mqtt_connected){ conn.innerHTML='<span class=ok>Connected</span>'; } else { conn.innerHTML='<span class=warn>Disconnected</span>'; } } $('mqtt_discovery_status').textContent=j.mqtt_discovery_status||'unknown'; const err=$('mqtt_err'); if(err){ err.textContent=j.mqtt_last_error||''; } }function updateTimeZoneVisibility(){ const mode=Number($('sel_time_zone_mode').value); $('row_manual_time_offset').style.display=mode===0?'':'none'; $('row_custom_time_zone').style.display=mode===9?'':'none'; $('sel_offset').disabled=mode!==0; $('btn_offset_set').disabled=mode!==0; if(mode!==0) $('row_offset_hint').style.display='none'; }
function openSection(id){const el=$(id);if(el)el.open=true;}
const overviewState={};
function updateOverview(kind,j){
 overviewState[kind]=j;
 if(kind==='status'&&typeof updateNoiseSave==='function')updateNoiseSave();
 const put=(id,text,cls='')=>{const el=$(id);if(el){el.textContent=text;el.className=cls;}};
 if(kind==='status'){
  put('overview_wifi','Wi-Fi: '+j.wifi_rssi+' dBm');put('overview_clients','RTSP clients: '+((j.s1_clients||0)+(j.s2_clients||0)));put('overview_uptime','Uptime: '+j.uptime);
  renderBattery(j);
  renderFieldPower(j);
  const battery=j.battery,indicator=$('overview_battery');
  if(indicator){
   indicator.hidden=!battery?.enabled||!battery?.supported;
   const valid=battery?.valid&&Number.isFinite(battery.voltage)&&Number.isFinite(battery.percent);
   put('overview_battery',valid?`Battery: ~${battery.percent}% · ${battery.voltage.toFixed(2)} V`:'Battery: unavailable',valid?(battery.percent<=10?'bad':battery.percent<=20?'warn':''):'warn');
  }
 }
 if(kind==='audio'){
  put('overview_rate',(j.sample_rate/1000)+' kHz');
  put('overview_level','Level: '+(Number.isFinite(j.peak_dbfs)?j.peak_dbfs.toFixed(1)+' dBFS':'—')+(j.clip?' · CLIPPING':''),j.clip?'bad':'');
  const state=j.mic_signal,warn=state==='flatline';
  put('overview_mic','Microphone: '+(state==='ok'?'OK':state||'unavailable'),warn?'bad':state==='ok'?'ok':'muted');
  $('microphone_notice').hidden=!warn;put('microphone_notice_detail',$('mic_signal_status').textContent);
 }
 if(kind==='thermal')put('overview_temp',j.current_valid&&Number.isFinite(j.current_c)?'Temperature: '+j.current_c.toFixed(1)+' °C':'Temperature: unavailable',j.latched_persist||j.sensor_fault?'bad':'');
 const status=overviewState.status,thermal=overviewState.thermal;
 if(!status)return;
 let text='',target='streamssec',cls='';
 if(thermal?.latched_persist){text=thermal.auto_resume_pending?'Paused — cooling down':'Paused — thermal acknowledgement required';target='thermalsec';cls='warn';}
 else if(status.stream_schedule_enabled&&status.stream_schedule_time_valid&&!status.stream_schedule_allow_now){text='Paused by schedule until '+minsToHHMM(status.stream_schedule_start_min);target='timesec';}
 else if(!status.rtsp_server_enabled)text='RTSP server off';
 else if(!status.stream1_enabled&&!status.stream2_enabled)text='Both streams disabled';
 else {const n=Number(!!status.s1_streaming)+Number(!!status.s2_streaming);text=n?'RTSP: '+n+' streaming':'Ready — waiting for a client';}
 if(status.stream_schedule_enabled&&!status.stream_schedule_time_valid)text+=' · schedule waiting for clock';
 put('overview_streams',text,cls);const link=$('overview_streams');link.href='#'+target;link.onclick=()=>openSection(target);
}
function loadStatus(){return fetchJSON('/api/status').then(j=>{ renderAntenna(j); renderRecording(j); renderDeviceSetup(j); updateOverview('status',j); $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); setToggleState(j.rtsp_server_enabled); $('s1_client').textContent=(j.s1_clients>0)?j.s1_clients+' client(s)':j.stream1_enabled?'Waiting...':'Disabled'; $('s1_stream').innerHTML=fmtBool(j.s1_streaming); $('s1_rate').textContent=j.s1_pkt_rate+' pkt/s'; $('s1_lplay').textContent=j.s1_last_play||'never'; $('s2_client').textContent=(j.s2_clients>0)?j.s2_clients+' client(s)':(j.stream2_enabled?'Waiting...':'Disabled'); $('s2_stream').innerHTML=j.stream2_enabled?fmtBool(j.s2_streaming):'<span class=muted>N/A</span>'; $('s2_rate').textContent=j.stream2_enabled?(j.s2_pkt_rate+' pkt/s'):'—'; $('s2_lplay').textContent=j.stream2_enabled?(j.s2_last_play||'never'):'—'; $('last_sync').textContent=j.last_time_sync; renderTimeRows(j); const now=Date.now(); const stx=$('sel_tx'); syncInput(stx,'wifi_tx',j.wifi_tx_dbm.toFixed(1)); const md=$('sel_mdns'); syncInput(md,'mdns_enable',j.mdns_enabled?'on':'off'); const mh=$('in_mdns_host'); syncInput(mh,'mdns_hostname',j.mdns_hostname||''); const tse=$('sel_time_sync'); syncInput(tse,'time_sync',j.time_sync_enabled?'on':'off'); const zoneSel=$('sel_time_zone_mode'); syncInput(zoneSel,'time_zone_mode',String(j.time_zone_mode||0)); updateTimeZoneVisibility(); const ruleInput=$('in_time_zone_rule'); syncInput(ruleInput,'time_zone_rule',j.time_zone_rule||''); const offSel=$('sel_offset'); if(offSel)syncInput(offSel,'time_offset',String((j.time_offset_min||0)/60)); const fv=$('fwv'); if(fv && j.fw_version){ fv.textContent='v'+j.fw_version; } const ui=$('url_ip'); if(ui){ ui.textContent=j.stream1_url_ip; ui.href=j.stream1_url_ip; } const um=$('url_mdns'); const umw=$('url_mdns_wrap'); if(um){ if(j.mdns_enabled){ um.textContent=j.stream1_url_mdns; um.href=j.stream1_url_mdns; um.style.opacity='1'; if(umw) umw.style.display=''; } else { um.textContent='—'; um.removeAttribute('href'); um.style.opacity='0.6'; if(umw) umw.style.display='none'; } } const u2=$('url2_ip'); if(u2){ u2.textContent=j.stream2_url_ip; u2.href=j.stream2_url_ip; } const u2m=$('url2_mdns'); const u2mw=$('url2_mdns_wrap'); if(u2m){ if(j.mdns_enabled){ u2m.textContent=j.stream2_url_mdns; u2m.href=j.stream2_url_mdns; u2m.style.opacity='1'; if(u2mw) u2mw.style.display=''; } else { u2m.textContent='—'; u2m.removeAttribute('href'); u2m.style.opacity='0.6'; if(u2mw) u2mw.style.display='none'; } } const s1t=$('sel_stream1_target'); syncInput(s1t,'stream1_target',String(j.stream1_target||0)); const s2t=$('sel_stream2_target'); syncInput(s2t,'stream2_target',String(j.stream2_target||0)); const s1e=$('sel_stream1_enabled'); syncInput(s1e,'stream1_enabled',j.stream1_enabled?'on':'off'); const s2e=$('sel_stream2_enabled'); syncInput(s2e,'stream2_enabled',j.stream2_enabled?'on':'off'); const s1tp=$('s1_transport'); if(s1tp) s1tp.textContent=(j.stream1_target===0)?'TCP':'UDP'; const s2tp=$('s2_transport'); if(s2tp) s2tp.textContent=(j.stream2_target===0)?'TCP':'UDP'; const mc=$('sel_max_clients'); syncInput(mc,'max_clients',String(j.max_clients||2)); loadStreamSchedule(j); loadMqtt(j); })}function renderAudioLevel(j){
 const lvl=$('level'); if(!lvl) return;
 const L=T[lang]||T.en;
 const pct=typeof j.peak_pct==='number'&&Number.isFinite(j.peak_pct)?j.peak_pct:0;
 const db=typeof j.peak_dbfs==='number'&&Number.isFinite(j.peak_dbfs)?j.peak_dbfs:-90;
 const cc=Number.isFinite(j.clip_count)?j.clip_count:0;
 const text=`Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS)`;
 if(j.clip) lvl.innerHTML=`<span class='bad'>${j.mic_format===2?'CLIPPING! Reduce Gain.':L.clip_bad}</span> ${text}, clips: ${cc}`;
 else if(pct>=90) lvl.innerHTML=`<span class='warn'>${j.mic_format===2?'Clipping: reduce Gain.':L.clip_warn}</span> ${text}`;
 else lvl.textContent=text+' — '+L.clip_ok;
}
function updateAudioDiagnosticsUI(j){
 const load=$('health_audio_load');
 if(load){
   const valid=j.audio_cpu_valid===true && typeof j.audio_cpu_pct==='number' && Number.isFinite(j.audio_cpu_pct) &&
     typeof j.audio_cpu_peak_pct==='number' && Number.isFinite(j.audio_cpu_peak_pct);
   load.className='v';
   if(valid){
     load.textContent=j.audio_cpu_pct.toFixed(1)+'% (peak '+j.audio_cpu_peak_pct.toFixed(1)+'%)';
     if(j.audio_cpu_pct>=80) load.className+=' bad'; else if(j.audio_cpu_pct>=60) load.className+=' warn';
   } else load.textContent='Unavailable';
 }
 const dma=$('health_dma');
 if(dma){
   const valid=j.dma_overruns_available===true && Number.isInteger(j.dma_overruns_1min) && Number.isInteger(j.dma_overruns_total);
   dma.className='v';
   dma.textContent=valid?j.dma_overruns_1min+' / '+j.dma_overruns_total+' (last: '+(j.dma_overrun_last_age||'never')+')':'Unavailable';
   if(valid && j.dma_overruns_1min>0) dma.className+=' warn';
 }
}
function updateMicrophoneSignal(j){
 const el=$('mic_signal_status'); if(!el) return;
 const state=j.mic_signal;
 el.className=state==='flatline'?'bad':state==='ok'?'ok':'muted';
 el.textContent=state==='flatline'?'Microphone signal unchanged for '+(Number(j.mic_flatline_sec)||20)+' s. Check microphone wiring and power.':
   state==='ok'?'Microphone signal: OK':state==='checking'?'Microphone signal: checking…':'Microphone signal: unavailable';
}
function updateStreamHealth(j){
updateMicrophoneSignal(j);
updateAudioDiagnosticsUI(j);
const capture=$('health_capture');if(capture)capture.textContent=(j.producer_running?'Running':'Paused')+' / '+(j.i2s_error_count??0);
$('health_stalls').textContent=j.rtsp_write_stalls??0;
$('health_failures').textContent=(j.rtsp_write_failures??0)+' / '+(j.rtsp_write_timeouts??0);
$('health_clients').textContent=(j.rtsp_client_teardowns??0)+' / '+(j.rtsp_client_disconnects??0);
$('health_buffer').textContent=(j.rb_drops??0)+' / '+(j.rb_flushes??0);
$('health_stop').textContent=j.last_stop_time ? (j.last_stop_stream?'Stream '+j.last_stop_stream:'All streams')+' — '+j.last_stop_reason+' — '+j.last_stop_time+' ('+j.last_stop_age+')' : 'No stop recorded';
}
async function saveS3Board(){
 if(micDialogLoading||micDialogState||sectionBusy.has('audiosec')||sectionBusy.has('noisesec'))return;
 const variant=Number($('sel_s3_board').value);
 if(!differsFromSaved('s3_board'))return;
 const keys=['s3_board'];
 await saveValues('/api/settings/s3_board',{variant},keys,'audiosec','Board saved. Audio restarted. Check the signal level and adjust Gain if needed.');
}
let micDialogState=null,micDialogLoading=false;
const micProfileKeys=['mic_format','rate','gain','buffer','shift','hp_enable','hp_cutoff'];
async function chooseMicSettings(force=false){
 if(micDialogLoading||micDialogState||sectionBusy.has('audiosec')||sectionBusy.has('noisesec')||sectionBusy.has('thermalsec'))return;
 const format=Number($('sel_mic_format').value),label=$('sel_mic_format').selectedOptions[0]?.textContent||'';
 micDialogLoading=true;
 try{
  const [p,a,t]=await Promise.all([fetchJSON('/api/microphone_profile?format='+format),fetchJSON('/api/audio_status'),fetchJSON('/api/thermal')]);
  if(!p.ok||p.mic_format!==format)throw new Error('profile');
  if(Number($('sel_mic_format').value)!==format||sectionBusy.has('audiosec')||sectionBusy.has('noisesec')||sectionBusy.has('thermalsec'))return;
  if(!force&&a.mic_format===format){feedback('audiosec','This microphone is already selected. Use Recommended settings to review its defaults.');return;}
  const rows=$('mic_dialog_rows');rows.replaceChildren();
  const add=(label,old,value)=>{const tr=document.createElement('tr');for(const text of [label,old,value]){const td=document.createElement('td');td.textContent=String(text);tr.append(td);}rows.append(tr);};
  add('Sample rate',a.sample_rate+' Hz',p.sample_rate+' Hz');
  add('Gain',a.gain,p.gain);add('Audio buffer size',a.buffer_size+' samples',p.buffer_size+' samples');
  add('I2S Shift',a.mic_format===2?'Unused':a.i2s_shift,format===2?'Unused':p.i2s_shift);
  add('High-pass filter',a.hp_enable?'On':'Off',p.hp_enable?'On':'Off');
  add('HPF cutoff',a.hp_cutoff_hz+' Hz',p.hp_cutoff_hz+' Hz');
  if(p.cpu_mhz)add('CPU frequency',t.cpu_mhz+' MHz',p.cpu_mhz+' MHz');
  $('mic_dialog_target').textContent=p.board+' · '+label;
  $('mic_dialog_note').textContent=format===2?'Gain '+p.gain+' is a starting point. Check the level with Listen and reduce Gain if clipping occurs.':'These are starting values. Use Listen to check the level and adjust Gain for your microphone and location.';
  $('mic_dialog_drafts').textContent=micProfileKeys.concat(p.cpu_mhz?['cpu_freq']:[]).some(k=>edits[k])?'Using recommendations replaces unsaved edits for the settings shown above. Keeping saved values leaves other edits pending.':'';
  micDialogState={format,profile:p};$('mic_dialog').showModal();$('mic_dialog').scrollTop=0;
 }catch(_){micDialogState=null;feedback('audiosec','Could not load microphone recommendations. Check the connection and try again.','bad');}
 finally{micDialogLoading=false;}
}
async function saveMicSettings(mode){
 const state=micDialogState;if(!state)return;
 micDialogState=null;$('mic_dialog').close();
 const keys=mode==='recommended'?micProfileKeys.filter(k=>k!=='shift'||state.format!==2):['mic_format'];
 // A saved custom sample-rate draft cannot be used while PDM locks it to 48 kHz.
 if(state.format===2&&!keys.includes('rate'))keys.push('rate');
 if(mode==='recommended'&&state.profile.cpu_mhz)keys.push('cpu_freq');
 const cpuChange=mode==='recommended'&&state.profile.cpu_mhz;
 const relatedSections=['noisesec',...(cpuChange?['thermalsec']:[])];
 for(const id of relatedSections){sectionBusy.add(id);updateDiscard($(id));}
 if(typeof updateFeatureDependencies==='function')updateFeatureDependencies();
 else if(window.updateNoiseSave)updateNoiseSave();
 try{await saveValues('/api/settings/microphone',{format:state.format,settings:mode},keys,'audiosec',mode==='recommended'?'Microphone and recommended settings saved.':'Microphone saved. Custom values preserved.');}
 finally{for(const id of relatedSections){sectionBusy.delete(id);updateDiscard($(id));}if(typeof updateFeatureDependencies==='function')updateFeatureDependencies();else if(window.updateNoiseSave)updateNoiseSave();}
}
$('mic_dialog').addEventListener('close',()=>{micDialogState=null;});
function renderMicFormat(j){
  const row=document.getElementById('row_s3_board');
  if(row){
    row.style.display=j.s3_board_supported===true?'':'none';
    syncInput(document.getElementById('sel_s3_board'),'s3_board',String(j.s3_board??0));
  }
  const codec=j.audio_codec==='ES8311';
  const locked=j.s3_board_supported===true&&Number(j.s3_board)===2;
  const apply=document.getElementById('btn_mic_apply');
  if(apply)apply.disabled=false;

  const select=document.getElementById('sel_mic_format');
  const now=Date.now();
  if(select){select.disabled=codec;if(codec)select.options[0].textContent='Built-in microphone (ES8311)';for(const option of select.options||[])option.disabled=codec?Number(option.value)!==0:(Number(option.value)<2&&locked);syncInput(select,'mic_format',String(j.mic_format));}
  if(select){toggleDirty(select,'mic_format');document.getElementById('mic_format_label').textContent=codec?'Built-in microphone (ES8311)':(select.selectedOptions[0]?.textContent||'');}
  const option=document.getElementById('opt_pdm');
  if(option){
    option.disabled=codec||j.pdm_supported!==true;
    option.textContent=j.s3_board_supported===true&&Number(j.s3_board)!==0?'Built-in PDM microphone':'PDM microphone (IM73D122)';
  }
  if(codec){const r=document.getElementById('in_rate');if(r)r.max='48000';}
  const pdm=j.mic_format===2;
  const gainHint=document.getElementById('txt_gain_hint');
  if(gainHint) gainHint.textContent=pdm?'Adjust Gain to avoid clipping. PDM produces 16-bit PCM; I2S Shift is unused.':(T[lang]||T.en).help_gain;
  for(const id of ['in_rate','btn_rate_set','in_shift','btn_shift_set']){
    const el=document.getElementById(id);
    if(el){el.disabled=pdm; el.title=pdm?'PDM uses 48 kHz PCM; I2S Shift is unused.':'';}
  }
}
function loadAudio(){return fetchJSON('/api/audio_status').then(j=>{ const r=$('in_rate'); const g=$('in_gain'); const sb=$('sel_buf'); const s=$('in_shift'); const hp=$('sel_hp'); const hpc=$('in_hp_cutoff'); const now=Date.now(); syncInput(r,'rate',j.sample_rate); syncInput(g,'gain',j.gain.toFixed(2)); syncInput(sb,'buffer',j.buffer_size); syncInput(s,'shift',j.i2s_shift); syncInput(hp,'hp_enable',j.hp_enable?'on':'off'); syncInput(hpc,'hp_cutoff',j.hp_cutoff_hz); $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=profileText(j.buffer_size); renderMicFormat(j); if(window.renderNoiseSettings)renderNoiseSettings(j.noise); renderAudioLevel(j); updateStreamHealth(j); updateOverview('audio',j); updateAdvice(j); })}function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size_recommended===512){if(a.buffer_size!==512) tips.push('Use 512 samples on this board to leave RAM for Wi-Fi.');}else{if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024);} if(a.gain>20) tips.push(a.mic_format===2?'High PDM gain may clip. Reduce Gain if needed.':L.adv_gain); $('adv').textContent=tips.join(' ');}function loadPerf(){return fetchJSON('/api/perf_status').then(j=>{ const now=Date.now(); const el=$('in_auto'); syncInput(el,'auto_recovery',j.auto_recovery?'on':'off'); const thr=$('in_thr'); const chk=$('in_chk'); const mode=$('in_thr_mode'); const sch=$('in_sched'); const hrs=$('in_hours'); syncInput(mode,'thr_mode',j.auto_threshold?'auto':'manual'); syncInput(thr,'min_rate',j.restart_threshold_pkt_s); syncInput(chk,'check_interval',j.check_interval_min); syncInput(sch,'sched_reset',j.scheduled_reset?'on':'off'); syncInput(hrs,'reset_hours',j.reset_hours); $('row_min_rate').style.display=j.auto_threshold?'none':''; })}let thermalAutoPending=false;
function renderThermalRecovery(j){
 thermalAutoPending=!!j.auto_resume_pending;
 const stop=$('b_srv_off'); if(stop&&thermalAutoPending) stop.disabled=false;
 const now=Date.now(), auto=$('sel_oh_auto');
 if(auto)syncInput(auto,'oh_auto',j.auto_resume_enabled?'on':'off');
 const latched=!!j.latched_persist, notice=!!j.notice;
 const count=$('therm_trip_count'); if(count) count.textContent=String(j.trips_24h||0)+' / 3';
 const limit=Number(j.cool_limit_c).toFixed(0), seconds=Math.max(0,Number(j.cool_remaining_sec)||0);
 const time=Math.floor(seconds/60)+':'+String(seconds%60).padStart(2,'0');
 let title='Thermal shutdown recorded', message='', status='Ready';
 if(!j.protection_enabled){ status='Protection disabled'; }
 else if(j.sensor_fault){ status='Sensor unavailable — recovery blocked'; }
 else if(latched && j.auto_resume_pending){ status='Waiting for cooling'; }
 else if(latched){ status='Manual acknowledgement required'; }
 if(latched){
   title=j.emergency?'Emergency thermal shutdown — acknowledgement required':j.manual_blocked?'Repeated overheating — automatic recovery blocked':'Thermal shutdown — stream paused';
   message=!j.protection_enabled?'Thermal protection is off. Acknowledge to restore operation without waiting for cooling.':j.sensor_fault?'Temperature sensor unavailable. Recovery remains blocked.':
     'Recovery requires 5 continuous minutes at or below '+limit+' °C. Cooling time remaining: '+time+'.';
   if(j.auto_resume_pending){ message+=j.recording_mode?(j.recording_resume_pending?' Recording will continue in a new file after cooling.':' Microphone capture will resume after cooling.'):' The stream will resume automatically after cooling during the allowed schedule.'; if(!j.recording_mode&&!j.schedule_allowed) message+=' Currently outside the allowed schedule.'; }
   else if(j.protection_enabled){ message+=' Check cooling, then acknowledge to restore operation.'; }
 } else if(j.auto_resumed){ title='Thermal shutdown — operation automatically restored'; message='The device cooled down and operation resumed. Check cooling. This warning remains until acknowledged.'; }
 else { message='Check cooling. This warning remains until acknowledged.'; }
 if(j.last_trip_c>0) message+=' Last shutdown: '+Number(j.last_trip_c).toFixed(1)+' °C.';
 const banner=$('thermal_notice'); if(banner) banner.style.display=notice?'':'none';
 const titleEl=$('thermal_notice_title'); if(titleEl) titleEl.textContent=title;
 const detail=$('thermal_notice_detail'); if(detail) detail.textContent=message;
 const row=$('row_therm_latch'); if(row) row.style.display=notice?'':'none';
 const msg=$('txt_therm_latch'); if(msg) msg.textContent=message;
 const statusEl=$('therm_status'); if(statusEl){ statusEl.textContent=status; statusEl.className=latched||j.sensor_fault?'warn':!j.protection_enabled?'bad':'ok'; }
 for(const id of ['btn_therm_clear','btn_thermal_notice_ack']){ const btn=$(id); if(btn){ btn.textContent=latched?'Acknowledge and restore operation':'Acknowledge warning'; btn.disabled=!j.can_acknowledge; } }
}
function loadTherm(){return fetchJSON('/api/thermal').then(j=>{ const now=Date.now(); const L=T[lang]; const en=$('sel_oh_enable'); syncInput(en,'oh_enable',j.protection_enabled?'on':'off'); const lim=$('sel_oh_limit'); syncInput(lim,'oh_limit',(Number(j.shutdown_c)||80).toFixed(0)); const sc=$('sel_cpu'); for(const mhz of [240,320]){const fast=sc.querySelector('option[value="'+mhz+'"]');if(j.cpu_max_mhz>=mhz&&!fast)sc.add(new Option(String(mhz),String(mhz)));else if(!(j.cpu_max_mhz>=mhz)&&fast)fast.remove();} syncInput(sc,'cpu_freq',j.cpu_mhz); const currentValid=(j.current_valid&&typeof j.current_c==='number'&&isFinite(j.current_c)); const cur=$('therm_now'); if(cur) cur.textContent=currentValid?j.current_c.toFixed(1)+' °C':'N/A'; const max=$('therm_max'); if(max){ const maxValid=(typeof j.max_c==='number'&&isFinite(j.max_c)); max.textContent=maxValid?j.max_c.toFixed(1)+' °C':'N/A'; } const cpu=$('therm_cpu'); if(cpu) cpu.textContent=j.cpu_mhz+' MHz'; renderThermalRecovery(j); updateOverview('thermal',j); const last=$('therm_last'); if(last){ if(j.sensor_fault){ last.textContent=L.therm_last_sensor_fault; } else if(j.last_trip_ts && j.last_trip_ts.length){ let msg=L.therm_last_fmt; const temp=(typeof j.last_trip_c==='number'&&isFinite(j.last_trip_c)&&j.last_trip_c>0)?j.last_trip_c.toFixed(1):'0'; const limit=(Number(j.shutdown_c)||0).toFixed(0); const ts=j.last_trip_ts||L.therm_time_unknown; const ago=j.last_trip_since||L.therm_time_ago_unknown; msg=msg.replace('%TEMP%',temp).replace('%LIMIT%',limit).replace('%TIME%',ts).replace('%AGO%',ago); last.textContent=msg; if(j.latched_persist){ last.textContent+=' — '+(j.auto_resume_pending?'Waiting for cooling':L.therm_status_latched_persist); } else if(j.manual_restart){ last.textContent+=' — '+L.therm_status_latched; } } else if(j.last_reason && j.last_reason.length){ last.textContent=j.last_reason; } else { last.textContent=L.therm_last_none; } } })}let logLatestPage=true,logPageBusy=false,logPageState=null;
function updateLogPageControls(){
 const state=logPageState;
 const older=$('b_log_older'),newer=$('b_log_newer');
 if(older)older.disabled=logPageBusy||!state||state.start<=state.first;
 if(newer)newer.disabled=logPageBusy||!state||state.end>=state.last;
}
function fetchLogPage(direction){
 const lg=$('logs');if(!lg||logPageBusy)return Promise.resolve();
 bindLogScroll();let query='/api/logs?page=1';
 if(direction==='older'&&logPageState)query+='&before='+logPageState.start;
 if(direction==='newer'&&logPageState)query+='&after='+logPageState.end;
 logPageBusy=true;updateLogPageControls();
 return fetch(query,{cache:'no-store',signal:AbortSignal.timeout(8000)}).then(async response=>{
  if(!response.ok)throw new Error('Device logs unavailable');
  const text=await response.text();
  const get=name=>response.headers?.get?.(name);
  const start=get('X-ESP32MIC-Log-Start'),end=get('X-ESP32MIC-Log-End'),first=get('X-ESP32MIC-Log-First'),last=get('X-ESP32MIC-Log-Last');
  const pinBottom=logAutoScroll||isLogNearBottom(lg),bottomOffset=lg.scrollHeight-lg.scrollTop;
  lg.textContent=text;
  if(start!==null&&start!==undefined&&end!==null&&first!==null&&last!==null){
   logPageState={start:Number(start),end:Number(end),first:Number(first),last:Number(last)};
   logLatestPage=logPageState.end>=logPageState.last;
   const capacity=Number(get('X-ESP32MIC-Log-Capacity'))||0,storage=get('X-ESP32MIC-Log-Storage');
   const status=$('log_page_status');
   if(status)status.textContent=(logLatestPage?'Latest messages':'Older messages')+' · '+Math.round(capacity/1024)+' KiB retained'+(storage==='psram'?' in PSRAM':'')+(get('X-ESP32MIC-Log-Moved')==='1'?' · History changed; showing available messages.':'');
  }
  if(direction==='older'||direction==='newer')lg.scrollTop=0;
  else if(pinBottom)lg.scrollTop=lg.scrollHeight;
  else lg.scrollTop=Math.max(0,lg.scrollHeight-bottomOffset);
 }).finally(()=>{logPageBusy=false;updateLogPageControls();});
}
function loadLogs(){if(!logLatestPage)return Promise.resolve();return fetchLogPage('latest');}
function loadOlderLogs(){if(logPageBusy)return;logLatestPage=false;return fetchLogPage('older').catch(()=>{$('log_page_status').textContent='Could not load older messages. Try again.';});}
function loadNewerLogs(){if(logPageBusy)return;return fetchLogPage('newer').catch(()=>{$('log_page_status').textContent='Could not load newer messages. Try again.';});}
function loadLatestLogs(){if(logPageBusy)return;logLatestPage=true;logAutoScroll=true;return fetchLogPage('latest').catch(()=>{$('log_page_status').textContent='Could not load messages. Try again.';});}
function downloadLogs(){ fetch('/api/logs?download=1',{cache:'no-store'}).then(r=>r.blob()).then(b=>{ const url=URL.createObjectURL(b); const a=document.createElement('a'); a.href=url; a.download='esp32mic-log.txt'; a.click(); setTimeout(()=>URL.revokeObjectURL(url),2000); }); }function setOffset(){ const sel=$('sel_offset'); if(!sel) return; const h=Number(sel.value); if(!Number.isFinite(h)||h < -12||h > 14) return; setv('time_offset', Math.round(h*60)); }function confirmNetReset(){ const L=T[lang]; if(confirm(L.net_reset_confirm||'Reset Wi-Fi settings and reboot?')){ act('network_reset'); } }const POLL_FAST_MS=3000, POLL_SLOW_MS=15000; let pollBusy=false,pollPromise=null,lastSlowPoll=0,lastLogPoll=0;var wifiOffIntent=false;const pollErrors={};function logsOpen(){ const d=document.getElementById('logssec'); return !!(d&&d.open); }function loadAll(force){
 if(wifiOffIntent)return Promise.resolve();
 force=force===true;if(pollBusy)return force?pollPromise.then(()=>loadAll(true)):pollPromise;
 pollBusy=true;const now=Date.now(),jobs=[['status',loadStatus],['audio',loadAudio]];
 if(force||now-lastSlowPoll>=POLL_SLOW_MS){lastSlowPoll=now;jobs.push(['perf',loadPerf],['thermal',loadTherm],['wifi',loadWifiAp]);}
 if(logsOpen()&&(force||now-lastLogPoll>=POLL_SLOW_MS)){lastLogPoll=now;jobs.push(['logs',loadLogs]);}
 pollPromise=Promise.allSettled(jobs.map(async([key,load])=>{try{await load();delete pollErrors[key];}catch(error){pollErrors[key]=(pollErrors[key]||0)+1;throw error;}})).then(()=>{
  if(wifiOffIntent)return;
  const failing=key=>(pollErrors[key]||0)>=2;
  $('connection_status').textContent=Object.keys(pollErrors).some(failing)?'Some device data could not be refreshed. Displayed values may be out of date.':'';
  if(failing('status')){$('device_mode_status').textContent='Device status unavailable';$('device_mode_status').className='muted';$('overview_streams').textContent='RTSP: unavailable';$('overview_wifi').textContent='Wi-Fi: unavailable';}
  if(failing('audio')){$('overview_mic').textContent='Microphone: unavailable';$('overview_mic').className='muted';}
  if(failing('thermal')){$('overview_temp').textContent='Temperature: unavailable';$('overview_temp').className='muted';}
  updateDependencies();
 }).finally(()=>{pollBusy=false;pollPromise=null;});return pollPromise;
}
function refreshAll(){ return loadAll(true); }function schedulePoll(){ loadAll(false).finally(()=>setTimeout(schedulePoll,POLL_FAST_MS)); }function startPolling(){ refreshAll().finally(()=>setTimeout(schedulePoll,POLL_FAST_MS)); }function clearThermalLatch(){
 for(const id of ['btn_therm_clear','btn_thermal_notice_ack']){ const btn=$(id); if(btn) btn.disabled=true; }
 return mutationFetch('/api/thermal/clear','').then(r=>r.json()).then(j=>{
   if(!j.ok) alert(j.error==='cooling_required'?'The device must finish cooling before acknowledgement.':'Could not acknowledge the thermal warning. Refresh and try again.');
   return refreshAll();
 }).catch(()=>refreshAll());
}startPolling();const tsel=$('themeSel'); if(tsel){ tsel.value=theme; tsel.onchange=()=>{theme=tsel.value; try{localStorage.setItem('theme',theme);}catch(_){} applyTheme();}; } applyTheme(); bindSaver($('in_rate'),'rate'); bindSaver($('in_gain'),'gain'); bindSaver($('in_shift'),'shift'); bindSaver($('in_thr'),'min_rate'); bindSaver($('in_chk'),'check_interval'); bindSaver($('in_hours'),'reset_hours'); bindSaver($('in_hp_cutoff'),'hp_cutoff'); bindEnter($('in_mdns_host'),()=>setvRaw('mdns_hostname',$('in_mdns_host').value));bindEnter($('in_mqtt_host'),saveMqttConnection); bindEnter($('in_mqtt_port'),saveMqttConnection); bindEnter($('in_mqtt_user'),saveMqttConnection); bindEnter($('in_mqtt_pass'),saveMqttConnection); bindEnter($('in_mqtt_topic'),setMqttTopic); bindEnter($('in_mqtt_discovery'),setMqttDiscovery); bindEnter($('in_mqtt_client_id'),setMqttClientId); bindEnter($('in_mqtt_interval'),setMqttInterval);trackEdit($('in_rate'),'rate'); trackEdit($('in_gain'),'gain'); trackEdit($('in_shift'),'shift'); trackEdit($('in_thr'),'min_rate'); trackEdit($('in_chk'),'check_interval'); trackEdit($('in_hours'),'reset_hours'); trackEdit($('in_hp_cutoff'),'hp_cutoff');trackEdit($('in_auto'),'auto_recovery'); trackEdit($('in_thr_mode'),'thr_mode'); trackEdit($('in_sched'),'sched_reset'); trackEdit($('sel_buf'),'buffer'); trackEdit($('sel_tx'),'wifi_tx'); trackEdit($('sel_hp'),'hp_enable'); trackEdit($('sel_cpu'),'cpu_freq'); trackEdit($('sel_oh_auto'),'oh_auto');trackEdit($('sel_oh_enable'),'oh_enable'); trackEdit($('sel_oh_limit'),'oh_limit');trackEdit($('sel_mdns'),'mdns_enable'); trackEdit($('in_mdns_host'),'mdns_hostname'); trackEdit($('sel_time_zone_mode'),'time_zone_mode'); $('sel_time_zone_mode').addEventListener('change',updateTimeZoneVisibility); trackEdit($('in_time_zone_rule'),'time_zone_rule'); trackEdit($('sel_offset'),'time_offset'); trackEdit($('sel_time_sync'),'time_sync'); trackEdit($('sel_stream_sched_en'),'stream_sched'); trackEdit($('in_stream_start'),'stream_start_min'); trackEdit($('in_stream_stop'),'stream_stop_min'); trackEdit($('sel_deep_sleep_sched'),'deep_sleep_sched'); trackEdit($('sel_mqtt_en'),'mqtt_enable');
trackEdit($('sel_stream1_enabled'),'stream1_enabled');
trackEdit($('sel_stream2_enabled'),'stream2_enabled');
trackEdit($('sel_stream1_target'),'stream1_target');
trackEdit($('sel_stream2_target'),'stream2_target');
trackEdit($('sel_max_clients'),'max_clients'); trackEdit($('in_mqtt_host'),'mqtt_host'); trackEdit($('in_mqtt_port'),'mqtt_port'); trackEdit($('in_mqtt_user'),'mqtt_user'); trackEdit($('in_mqtt_pass'),'mqtt_pass'); trackEdit($('in_mqtt_topic'),'mqtt_topic'); trackEdit($('in_mqtt_discovery'),'mqtt_discovery'); trackEdit($('in_mqtt_client_id'),'mqtt_client_id'); trackEdit($('in_mqtt_interval'),'mqtt_interval');const H=(hid,rid)=>{const h=$(hid), r=$(rid); if(h&&r){ h.setAttribute('aria-controls',rid); h.onclick=()=>{ const open=r.style.display==='none'; r.style.display=open?'':'none'; h.setAttribute('aria-expanded',String(open)); }; }};H('h_max_clients','max_clients_hint'); H('h_rate','row_rate_hint'); H('h_gain','row_gain_hint'); H('h_hpf','row_hpf_hint'); H('h_hpf_cut','row_hpf_cut_hint'); H('h_buf','row_buf_hint'); H('h_auto','row_auto_hint'); H('h_thr','row_thr_hint'); H('h_thr_mode','row_thrmode_hint'); H('h_chk','row_chk_hint'); H('h_sched','row_sched_hint'); H('h_hours','row_hours_hint'); H('h_tx','row_tx_hint'); H('h_shift','row_shift_hint'); H('h_cpu','row_cpu_hint'); H('h_level','row_level_hint'); H('h_therm_protect','row_therm_hint_protect'); H('h_therm_limit','row_therm_hint_limit'); H('h_offset','row_offset_hint'); H('h_time_sync','row_time_sync_hint'); H('h_stream_sched_en','row_stream_sched_en_hint'); H('h_stream_start','row_stream_start_hint'); H('h_stream_stop','row_stream_stop_hint'); H('h_deep_sleep_sched','row_deep_sleep_sched_hint');
trackEdit($('mqtt_clear_password'),'mqtt_clear_password');
for(const id of ['in_stream_start','in_stream_stop'])bindEnter($(id),saveSchedule);
for(const id of ['sel_stream_sched_en','sel_hp','in_sched'])$(id).addEventListener('change',updateDependencies);
document.querySelectorAll('table tr').forEach(row=>{if(row.querySelector('input,select'))row.classList.add('setting-row');});
document.querySelectorAll('input,select').forEach(el=>{if(!el.labels?.length&&!el.getAttribute('aria-label')){const text=el.closest('tr')?.querySelector('td.k')?.textContent.replace('?','').trim();if(text)el.setAttribute('aria-label',text);}});
window.addEventListener('beforeunload',event=>{if(Object.keys(edits).length||wifiApDirty||antennaDirty||recordingDirty||recordingLogDirty||fieldPowerDirty||batteryDraft!==null){event.preventDefault();event.returnValue='You have unsaved changes.';}});
