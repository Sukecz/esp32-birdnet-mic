
let deviceSetupState=null,directAccessLoaded=false,directAccessLoading=false;
document.getElementById('audiosec').addEventListener('toggle',function(){
 if(this.open)document.getElementById('audioadvanced').open=true;
});
document.getElementById('networksec').addEventListener('toggle',function(){
 if(this.open)loadDirectAccess();
});
function renderDeviceSetup(j){
 const s=j.setup;if(!s)return;deviceSetupState=s;
 document.getElementById('direct_access_status').textContent=s.ap_active?'Active — join this network to open controls':'Standby — available when home Wi-Fi is unavailable';
 document.getElementById('direct_access_name').textContent=s.ap_name;
 if(document.getElementById('networksec').open)loadDirectAccess();
 document.getElementById('device_setup_summary').textContent='Startup: '+(s.default_mode==='recording'?'Recording':'Streaming')+' · '+(s.connection==='direct'?'Standalone Wi-Fi':s.connected?'Home Wi-Fi connected':'Waiting for home Wi-Fi')+(s.ap_active?' · Device Wi-Fi active':'');
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
