
let antennaState=null,antennaDirty=false,antennaBusy=false;
function antennaDraftChanged(){
 const choice=document.getElementById('wifi_antenna');
 antennaDirty=!!antennaState&&choice.value!==antennaState;
 document.getElementById('wifi_antenna_dirty').textContent=antennaDirty?'Unsaved':'';
 document.getElementById('wifi_antenna_save').disabled=!antennaDirty||antennaBusy;
}
function renderAntenna(status){
 document.getElementById('row_wifi_antenna').hidden=status.wifi_antenna_supported!==true;
 if(status.wifi_antenna_supported!==true)return;
 antennaState=status.wifi_antenna;
 if(!antennaDirty&&!antennaBusy)document.getElementById('wifi_antenna').value=antennaState;
 antennaDraftChanged();
}
async function saveAntenna(){
 if(antennaBusy||!antennaDirty)return;
 const requested=document.getElementById('wifi_antenna').value;
 if(!confirm('Save this antenna and restart? Selecting an antenna with a weaker signal may make the device unreachable.'))return;
 antennaBusy=true;antennaDraftChanged();document.getElementById('wifi_antenna').disabled=true;
 const message=document.getElementById('wifi_antenna_feedback');message.textContent='Saving…';
 try{
  const response=await mutationFetch('/api/settings/antenna',new URLSearchParams({antenna:requested}).toString());
  const result=await response.json();
  if(!response.ok||!result.ok)throw new Error('Could not save antenna selection.');
  antennaDirty=false;antennaState=requested;
  if(result.reboot_required){message.textContent='Saved. Restarting with the selected antenna…';rebootSequence('reboot');}
  else message.textContent='Saved. Antenna unchanged.';
 }catch(e){message.textContent=e.message||'Could not confirm antenna selection.';}
 finally{antennaBusy=false;document.getElementById('wifi_antenna').disabled=false;antennaDraftChanged();}
}
