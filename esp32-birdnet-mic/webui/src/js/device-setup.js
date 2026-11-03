
let deviceSetupState=null,directAccessLoaded=false,directAccessLoading=false;
document.getElementById('networksec').addEventListener('toggle',function(){
 if(this.open)loadDirectAccess();
});
function renderDeviceSetup(j){
 const s=j.setup;if(!s)return;deviceSetupState=s;
 const recording=j.recording?.mode==='recording',standalone=recording&&s.connection==='direct';
 const apStatus=s.ap_active?(!s.complete?'Active · Setup':standalone?'Active · Standalone':s.connected?'Active · Backup access':'Active · Home Wi-Fi unavailable'):'Off · Available as backup';
 document.getElementById('home_wifi_status').textContent=s.connected?'Connected · '+s.ip:'Not connected';
 document.getElementById('home_wifi_network_row').hidden=!s.connected;
 document.getElementById('home_wifi_network').textContent=s.connected?(s.home_ssid||s.ssid):'';
 document.getElementById('device_wifi_status').textContent=apStatus;
 document.getElementById('direct_access_status').textContent=apStatus;
 syncInput(document.getElementById('recording_connection'),'recording_connection',s.connection);
 document.getElementById('recording_connection').disabled=j.recording?.supported===false||!!j.wifi_off_pending||!!j.wifi_suspended;
 updateRecordingConnection();
 document.getElementById('direct_access_name').textContent=s.ap_name;
 if(document.getElementById('networksec').open)loadDirectAccess();
 document.getElementById('device_setup_summary').textContent='Mode after power-on: '+(s.default_mode==='recording'?'Recording':'Streaming')+'.';
 if(j.recording&&j.recording.mode==='streaming'&&!s.connected){
  const status=document.getElementById('device_mode_status');status.textContent='Streaming: Waiting for Wi-Fi';status.className='warn';
 }
 const choice=document.querySelector('#device_mode option[value=recording]');if(choice&&j.recording)choice.disabled=!j.recording.supported;
}
async function loadDirectAccess(){
 if(!deviceSetupState||directAccessLoaded||directAccessLoading)return;
 directAccessLoading=true;
 const message=document.getElementById('direct_access_password');
 try{
  const response=await mutationFetch('/api/setup/access','');const result=await response.json();
  if(!response.ok||!result.ok)throw Error();
  message.textContent=result.password;directAccessLoaded=true;window.deviceApPassword?.confirmed(result.password);
 }catch(_){message.textContent='Could not load. Reopen Network to retry.';}
 finally{directAccessLoading=false;}
}

trackEdit(document.getElementById('recording_connection'),'recording_connection');
function updateRecordingConnection(){
 const input=document.getElementById('recording_connection');
 document.getElementById('recording_connection_apply').disabled=!deviceSetupState||input.disabled||sectionBusy.has('networksec')||!differsFromSaved('recording_connection');
}
document.getElementById('recording_connection').addEventListener('change',updateRecordingConnection);
async function saveRecordingConnection(){
 if(!deviceSetupState||!differsFromSaved('recording_connection'))return;
 await saveValues('/api/setup/recording_connection',{connection:document.getElementById('recording_connection').value},['recording_connection'],'networksec','Recording connection saved.',{feedbackId:'recording_connection_feedback',refresh:loadStatus,rawErrors:true});
 updateRecordingConnection();
}
