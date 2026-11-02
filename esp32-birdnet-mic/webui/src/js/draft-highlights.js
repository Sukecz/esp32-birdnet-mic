
function friendlyBytes(n){const u=['B','KB','MB','GB'];let i=0;while(n>=1000&&i<3){n/=1000;i++;}return n.toLocaleString('en',{maximumFractionDigits:i?1:0})+' '+u[i];}
const draftGroups=[['recording_duration_mode','recording_hours','recording_segment','recording_auto'],['recording_log_level','recording_log_sd','recording_log_max'],['field_wifi_delay','field_guard','field_cutoff','field_confirm','field_hysteresis','field_sleep'],['battery_monitor']];
function highlightNewDrafts(){
 if(!recordingState||!fieldPowerState||!batteryState)return;
 const saved={recording_duration_mode:recordingState.duration_minutes?1:0,recording_hours:recordingState.duration_minutes,recording_segment:recordingState.segment_minutes,recording_auto:recordingState.auto_start,recording_log_level:Number(recordingLogState?.detailed),recording_log_sd:recordingLogState?.sd_enabled,recording_log_max:Number(((recordingLogState?.max_kib||0)*1024/1e6).toFixed(2)),field_wifi_delay:fieldPowerState.wifi_delay_seconds,field_guard:fieldPowerState.enabled,field_cutoff:fieldPowerState.cutoff_mv/1000,field_confirm:fieldPowerState.confirm_seconds,field_hysteresis:fieldPowerState.hysteresis_mv/1000,field_sleep:Number(fieldPowerState.sleep_after_save),battery_monitor:batteryState.enabled};
 for(const ids of draftGroups)for(const id of ids)syncInput(document.getElementById(id),id,saved[id]);
 for(const rootId of ['recording_panel','batterysec','logssec']){
  const root=document.getElementById(rootId);updateDiscard(root);
  const button=root.querySelector(':scope > .discard');button.dataset.discardDrafts='1';
  button.disabled=sectionBusy.has(rootId)||recordingPending||fieldPowerPending||batteryPending||recordingLogPending;
 }
}
for(const ids of draftGroups)for(const id of ids)trackEdit(document.getElementById(id),id);
document.addEventListener('input',highlightNewDrafts);document.addEventListener('change',highlightNewDrafts);
for(const name of ['renderRecording','renderBattery','renderFieldPower','renderRecordingLog']){const original=window[name];window[name]=function(...args){const result=original(...args);highlightNewDrafts();return result;};}
