const editVersions={}, confirmedValues={}, sectionBusy=new Set(), savingKeys=new Set(), pendingSyncKeys=new Set();
function feedback(section,text,kind='muted'){
 const root=$(section)||$('recoverysec');let box=root.querySelector(':scope > .feedback');
 if(!box){box=document.createElement('div');box.setAttribute('role','status');box.setAttribute('aria-live','polite');root.querySelector('summary').after(box);}
 box.className='feedback '+kind;box.textContent=text;
}
function updateDiscard(root){
 if(!root)return;let button=root.querySelector(':scope > .discard');
 if(!button){button=document.createElement('button');button.className='discard';button.textContent='Discard unsaved changes';button.onclick=()=>discardChanges(root.id);root.append(button);}
 button.hidden=!root.querySelector('.dirty');button.disabled=sectionBusy.has(root.id);
}
async function discardChanges(id){
 if(id==='networksec'&&window.deviceApPassword?.busy())return;
 if(id==='networksec')window.deviceApPassword?.cancel();
 if(sectionBusy.has(id))return;
 $(id).querySelectorAll('[data-dirty-key]').forEach(el=>{const k=el.dataset.dirtyKey;pendingSyncKeys.delete(k);clearDirty(k);if(el.type==='password')el.value='';if(el.type==='checkbox')el.checked=false;});
 feedback(id,'Changes discarded.');await refreshAll();updateDependencies();
}
const saveErrors={invalid_value:'The device rejected this value. Check its range and the selected microphone.',invalid_schedule:'Use different start and end times. Sleep requires an enabled schedule.',invalid_mqtt:'Check the broker address, port and password settings.',thermal_latched:'Thermal protection is active. Wait for cooling and acknowledge the warning.',i2s_restart_failed:'The microphone could not restart. Check its wiring and format.'};
async function saveValues(url,values,keys,section,success='Saved.'){
 if(sectionBusy.has(section))return false;
 const versions=keys.map(k=>editVersions[k]||0);
 keys.forEach(k=>{savingKeys.add(k);pendingSyncKeys.add(k);markDirty(k);});
 sectionBusy.add(section);updateDiscard($(section));feedback(section,'Saving…');
 try{
  const response=await mutationFetch(url,new URLSearchParams(values).toString());
  const result=await response.json();
  if(!response.ok||!result.ok)throw new Error(saveErrors[result.error]||'The device could not apply the change. Try again.');
  // Let older status requests finish while drafts are still protected.
  if(pollPromise)await pollPromise;
  keys.forEach((k,i)=>{if((editVersions[k]||0)===versions[i])clearDirty(k);});
  keys.forEach(k=>savingKeys.delete(k));
  feedback(section,success,'ok');await refreshAll();return true;
 }catch(error){feedback(section,error.message&&saveErrors&&Object.values(saveErrors).includes(error.message)?error.message:'Could not confirm the change. Your edits are kept; check the connection and try again.','bad');return false;}
 finally{
  // A response started before the write must not become the new saved baseline.
  if(pollPromise)await pollPromise;
  keys.forEach(k=>savingKeys.delete(k));
  sectionBusy.delete(section);updateDiscard($(section));
 }
}
