
let batteryState=null,batteryDraft=null,batteryPending=false,batteryDevice=null;
function batteryDraftChanged(){
 if(!batteryState)return;
 batteryDraft=document.getElementById('battery_monitor').checked;
 if(batteryDraft===!!batteryState.enabled)batteryDraft=null;
 renderBattery(batteryDevice);
}
function renderBattery(j){
 batteryDevice=j;batteryState=j.battery||{enabled:false,supported:false,valid:false};
 const b=batteryState,n=id=>document.getElementById(id),supported=!!b.supported;
 const family=j.chip_family||'',c5=family==='ESP32-C5';
 n('battery_board').textContent=(j.board_name||family||'Board')+(supported?'':' · Unsupported');
 n('battery_wiring').textContent=!supported?'Battery monitoring is unsupported on M5Stack AtomS3U.':c5?'No external divider is needed. Connect the battery directly to BAT+ and BAT−. The voltage divider is built into XIAO C5.':`Use two 100 kΩ resistors: BAT+ → 100 kΩ → D0/A0 (GPIO${family==='ESP32-C3'?2:family==='ESP32-S3'?1:0}), then 100 kΩ from D0/A0 to GND. Connect BAT− to GND. Never connect BAT+ directly to the ADC pin.`;
 n('battery_divider').hidden=!supported||c5;
 n('battery_pack_note').hidden=!supported;
 if(!supported)batteryDraft=null;
 if(batteryDraft===!!b.enabled)batteryDraft=null;
 n('battery_monitor').checked=supported&&(batteryDraft===null?!!b.enabled:batteryDraft);
 const locked=batteryPending||!!j.recording?.busy||!!j.recording?.cooling_pending;
 n('battery_monitor').disabled=!supported||locked||!!(j.field_power?.enabled&&b.enabled);let dependency=n('battery_monitor_dependency');if(!dependency){dependency=document.createElement('p');dependency.id='battery_monitor_dependency';dependency.className='section-note';n('battery_monitor').closest('.actions').after(dependency);}dependency.textContent=locked?'Stop recording before changing battery settings.':j.field_power?.enabled?(j.recording?.mode==='recording'?'To turn battery monitoring off, first disable low-battery protection below.':'Battery monitoring is required by the saved low-battery protection setting in Recording mode.'):'';
 n('battery_save').disabled=!supported||locked||batteryDraft===null;
 n('battery_dirty').textContent=batteryDraft===null?'':'Unsaved';
 const valid=b.valid&&Number.isFinite(b.voltage);
 n('battery_reading').textContent=!supported?'Unsupported':!b.enabled?'Monitoring disabled':valid?`${b.voltage.toFixed(2)} V · ~${b.percent}%`:'Voltage unavailable; waiting for a valid reading.';
 const correction=Number.isFinite(b.calibration_percent)?b.calibration_percent:0;
 n('battery_correction').textContent=`Applied correction: ${correction>=0?'+':''}${correction.toFixed(2)}%`;
 n('battery_reference').disabled=!supported||!b.enabled||locked;
 n('battery_calibrate').disabled=!supported||!b.enabled||!valid||locked||batteryDraft!==null;
 n('battery_reset').disabled=!supported||locked||correction===0;
}
async function batteryAction(action){
 if(!batteryState||batteryPending)return;
 const n=id=>document.getElementById(id),data={action};
 if(action==='enable'){
  if(batteryDraft===null)return;
  data.enabled=batteryDraft?'1':'0';
 }else if(action==='calibrate'){
  const reference=Number(n('battery_reference').value);
  if(!Number.isFinite(reference)||reference<2.5||reference>4.3){n('battery_feedback').textContent='Enter the measured voltage between 2.5 and 4.3 V.';return;}
  data.reference=String(reference);
 }
 batteryPending=true;renderBattery(batteryDevice);n('battery_feedback').textContent='Saving…';
 try{
  const response=await mutationFetch('/api/battery',new URLSearchParams(data).toString()),result=await response.json();
  if(!response.ok||!result.ok)throw Error(result.error||'Could not save. Try again.');
  if(action==='enable')batteryDraft=null;
  if(result.battery)renderBattery({...batteryDevice,battery:result.battery});
  n('battery_feedback').textContent=action==='calibrate'?'Calibration saved.':action==='reset'?'Calibration reset.':'Monitoring saved.';
  await loadStatus();
 }catch(error){n('battery_feedback').textContent=error.message||'Could not save. Try again.';}
 finally{batteryPending=false;renderBattery(batteryDevice);}
}
