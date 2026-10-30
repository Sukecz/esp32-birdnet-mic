'use strict';
const node=id=>document.getElementById(id);
let state=null,accessKey='',working=false,testing=false,scanning=false,finished=false,networkEdited=false,polling=false;
let testRevision=0,testRequestPending=false;
try{const t=localStorage.getItem('theme');if(t==='dark'||t==='light')document.documentElement.dataset.theme=t;}catch(_){}
async function request(path,values){
 let response,result;
 try{response=await fetch(path,{method:values===undefined?'GET':'POST',cache:'no-store',headers:values===undefined?{}:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8','X-ESP32MIC-CSRF':'1'},body:values===undefined?undefined:new URLSearchParams(values),signal:AbortSignal.timeout(8000)});
 result=await response.json();
 }catch(cause){const error=new Error('Connection interrupted. Rejoin the device Wi-Fi and try again.');error.transport=true;throw error;}
 if(!response.ok||!result.ok)throw new Error(result.error||'Request failed. Try again.');
 return result;
}
function message(id,text,error=false){node(id).textContent=text;node(id).classList.toggle('error',error);}
function controls(){
 if(!state)return;
 const atom=state.s3_board_supported&&node('variant').value==='2';
 const batterySupported=!atom&&(state.s3_board_supported||state.battery_supported!==false);
 node('battery_enabled').disabled=working||!batterySupported;
 if(!batterySupported)node('battery_enabled').checked=false;
 node('battery_help').disabled=!batterySupported;
 node('battery_note').textContent=batterySupported?'Show estimated charge and voltage at the top of the main UI in both modes.':'Battery monitoring is not supported on this board.';
 node('mode').querySelector('[value=recording]').disabled=atom;
 if(atom&&node('mode').value==='recording')node('mode').value='streaming';
 const recording=node('mode').value==='recording';
 if(!recording)node('connection').value='home';
 const direct=recording&&node('connection').value==='direct';
 node('mode_note').hidden=!atom;
 node('connection_field').hidden=!recording;
 node('home_fields').hidden=direct;
 node('connection_note').textContent=recording?(direct?'Connect directly to control the device. Audio stays on SD.':'Wi-Fi is for control. Audio stays on SD.'):'Wi-Fi carries the audio and gives access to controls.';
 node('fallback').textContent=direct?'No router or internet needed. Your phone can disconnect while recording.':`Wi-Fi unavailable? Join ${state.ap_name} to open controls. `+(recording?'Recording continues offline.':'Streaming to your home network resumes when Wi-Fi returns.');
 node('choices').disabled=working;
 node('ssid').disabled=testing;
 node('password').disabled=testing;
 node('scan').disabled=scanning||testing;
 node('connect').disabled=testing||scanning;
 for(const button of node('networks').querySelectorAll('button')){
  button.disabled=working||testing||scanning;
  button.setAttribute('aria-pressed',String(button.dataset.ssid===node('ssid').value));
 }
 node('finish').disabled=testing||(!direct&&(networkEdited||(!state.ssid&&state.test_state!=='connected')));
 node('later').hidden=direct||state.complete;
}
async function poll(){
 if(finished||polling||scanning)return;
 polling=true;
 const revision=testRevision,submitting=testRequestPending;
 try{
  const next=await request('/api/setup');
  // A response started before the current Connect request cannot describe it.
  if(revision!==testRevision||submitting||testRequestPending)return;
  const previousTest=state&&state.test_state;
  if(!state){
   state=next;
   node('board_name').textContent=next.board_name;
   node('variant_field').hidden=!next.s3_board_supported;
   node('board_name').hidden=next.s3_board_supported;
   node('variant').value=String(next.s3_board);
   node('antenna_field').hidden=!next.antenna_supported;
   node('antenna').value=next.antenna;
   node('battery_enabled').checked=!!next.battery_enabled;
   node('mode').value=next.default_mode;
   node('connection').value=next.connection;
   node('ssid').value=next.ssid;
   node('intro').textContent=next.complete?'Board, startup mode, Wi-Fi and battery.':'Welcome. Set up your device.';
   node('finish').textContent=next.complete?'Save changes':'Finish setup';
   node('cancel').hidden=!next.complete;
   node('ap_name').textContent=next.ap_name;
   node('access_details').open=!next.complete;
   testing=['queued','connecting','connected'].includes(next.test_state);
   try{const access=await request('/api/setup/access',{});accessKey=access.password;node('ap_password').textContent=accessKey;}catch(_){node('ap_password').textContent='Could not load. Reload before finishing.';}
   message('feedback','');
  }else state=next;
  if(testing||next.test_state==='error'&&previousTest!=='error'){
   if(next.test_state==='connected'){
    testing=false;networkEdited=false;node('password').value='';message('wifi_status','Connected. Finish setup to save this network.');
   }else if(next.test_state==='error'){
    testing=false;networkEdited=true;message('wifi_status',next.test_error==='trial_expired'?'Connection test expired. Test Wi-Fi again before saving.':'Could not connect. Check the network and password, then retry.',true);
   }else message('wifi_status','Connecting… If disconnected, rejoin the device Wi-Fi and reopen this page.');
  }
  controls();
 }catch(_){if(!state)message('feedback','Could not load settings. Retrying…',true);else if(testing)message('wifi_status',`Reconnecting… If needed, join ${state.ap_name} and open 192.168.4.1.`);}
 finally{polling=false;}
}
async function scan(){
 if(scanning||testing)return;
 scanning=true;controls();message('wifi_status','Searching for Wi-Fi…');
 try{
  // Never repeat POST after a lost acknowledgement: the scan may be running.
  const deadline=Date.now()+45000;
  try{await request('/api/setup/scan',{});}catch(e){if(!e.transport)throw e;}
  let result;
  while(Date.now()<deadline){
   await new Promise(r=>setTimeout(r,700));
   try{
    result=await request('/api/setup/scan');
    if(!result.scanning)break;
   }catch(e){
    if(!e.transport)throw e;
    message('wifi_status','Searching… waiting for the device Wi-Fi connection.');
   }
  }
  if(!result||result.scanning)throw new Error('Search timed out. Rejoin the device Wi-Fi and try again.');
  const list=node('networks');list.replaceChildren();
  const seen=new Set();
  for(const ap of result.aps||[]){
   if(!ap.ssid||seen.has(ap.ssid))continue;seen.add(ap.ssid);
   const button=document.createElement('button');button.type='button';button.className='network-choice';button.dataset.ssid=ap.ssid;
   const name=document.createElement('span');name.textContent=ap.ssid;
   const security=document.createElement('small');security.textContent=ap.secure?'Secured':'Open';button.append(name,security);
   button.onclick=()=>{
    if(node('ssid').value!==ap.ssid)node('password').value='';
    node('ssid').value=ap.ssid;networkEdited=true;controls();
    message('wifi_status',ap.secure?'Enter the password, then connect.':'Open network selected. Press Connect.');
    if(ap.secure)node('password').focus();
   };
   list.append(button);
  }
  list.hidden=!seen.size;
  message('wifi_status',seen.size?'Tap your Wi-Fi network below the name field.':'No networks found. Enter the name manually or try again.');
 }catch(e){message('wifi_status',e.message,true);}finally{scanning=false;controls();}
}
async function connect(){
 if(testing||scanning)return;
 const ssid=node('ssid').value,password=node('password').value;
 if(!ssid||new TextEncoder().encode(ssid).length>32){message('wifi_status','Enter a Wi-Fi name (up to 32 bytes).',true);return;}
 if(password.length&&password.length<8){message('wifi_status','Use at least 8 characters, or leave blank for an open network.',true);return;}
 ++testRevision;testRequestPending=true;testing=true;controls();message('wifi_status','Connecting…');
 try{await request('/api/setup/connect',{ssid,password});}catch(e){testing=false;message('wifi_status',e.message,true);controls();}
 finally{testRequestPending=false;}
}
async function finish(later=false){
 if(!state||working||testing)return;
 if(!accessKey){message('feedback','Reload to get your direct Wi-Fi password before finishing.',true);return;}
 working=true;controls();message('feedback','Saving…');
 try{
  const result=await request('/api/setup',{mode:node('mode').value,connection:node('connection').value,variant:node('variant').value,antenna:node('antenna').value,battery:node('battery_enabled').checked?'1':'0',allow_offline:later?'1':'0'});
  finished=true;node('setup_form').hidden=true;node('done').hidden=false;
  node('done_note').textContent=(result.reboot_required?'Restarting. ':'')+(state.complete?'Your default mode is saved for the next power-on.':'Setup complete. Other settings are in the main UI.');
  const direct=result.use_ap===undefined?node('connection').value==='direct':result.use_ap;
  node('done_access').textContent=`Direct access: ${state.ap_name} · Password: ${accessKey}. `+(!state.complete?'The setup network will now require this password. Reconnect if needed.':'');
  const link=node('open_controls');
  link.href=direct?'http://192.168.4.1/':state.connected&&state.ip!=='0.0.0.0'?`http://${state.ip}/`:'/';
  if(!direct&&state.connected)node('done_note').textContent+=' To use the home-network address, reconnect your phone to that Wi-Fi.';
  if(result.reboot_required)link.textContent='Open controls after restart';
 }catch(e){message('feedback',e.message||'Could not confirm save. Reload to check settings.',true);}
 finally{working=false;controls();}
}
function batteryWiring(){
 if(!state)return;
 const family=String(state.chip_family||'').toUpperCase();
 const c5=family==='ESP32-C5',c3=family==='ESP32-C3';
 const board=state.s3_board_supported?(node('variant').value==='1'?'XIAO ESP32-S3 Sense':'XIAO ESP32-S3'):state.board_name;
 node('battery_board').textContent=board;
 node('battery_wiring').textContent=c5?'Ready to use: the built-in divider has two 100 kΩ resistors. Connect the battery to BAT+ and BAT−; no external divider is needed.':`Connect BAT+ through 100 kΩ to D0/A0 (GPIO${c3?2:state.s3_board_supported?1:0}), then another 100 kΩ from A0 to GND. This input is separate from the microphone and microSD wiring.`;
 node('battery_diagram').hidden=c5;
 node('battery_dialog').showModal();
}
node('battery_help').onclick=batteryWiring;
node('battery_close').onclick=()=>node('battery_dialog').close();
node('battery_enabled').onchange=()=>{if(node('battery_enabled').checked)batteryWiring();};
for(const id of ['variant','antenna','mode','connection'])node(id).addEventListener('change',controls);
for(const id of ['ssid','password'])node(id).addEventListener('input',()=>{networkEdited=true;controls();});
node('show_password').onclick=()=>{const show=node('password').type==='password';node('password').type=show?'text':'password';node('show_password').textContent=show?'Hide':'Show';node('show_password').setAttribute('aria-pressed',String(show));};
node('copy_access').onclick=async()=>{
 const text=`${state.ap_name}\nPassword: ${accessKey}\nhttp://192.168.4.1`;
 try{
  if(!accessKey)throw Error();
  if(navigator.clipboard)await navigator.clipboard.writeText(text);
  else{
   const input=document.createElement('textarea');input.value=text;input.readOnly=true;input.style.position='fixed';input.style.opacity='0';document.body.append(input);input.select();input.setSelectionRange(0,text.length);
   const copied=document.execCommand('copy');input.remove();node('copy_access').focus();if(!copied)throw Error();
  }
  message('copy_status','Copied.');
 }catch(_){message('copy_status','Copy the network name and password shown above.');}
};
node('scan').onclick=scan;node('connect').onclick=connect;
node('setup_form').onsubmit=e=>{e.preventDefault();finish();};node('later').onclick=()=>finish(true);
poll().finally(()=>setInterval(poll,1500));
