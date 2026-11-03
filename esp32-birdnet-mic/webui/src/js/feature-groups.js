// One hierarchy for feature switches and their dependent settings across the UI.
// Grouped forms deliberately omit data-feature-enable so values can be prepared.
function featureEnabled(parentId,enabled='on'){
 const parent=document.getElementById(parentId),key=parent?.dataset.dirtyKey;
 return !!parent&&parent.dataset.savedLoaded==='1'&&String(fieldValue(parent))===enabled&&
  String(confirmedValues[key])===enabled&&!savingKeys.has(key)&&!pendingSyncKeys.has(key);
}
function updateFeatureDependencies(){
 if(typeof wifiOffIntent!=='undefined'&&wifiOffIntent)return;
 document.querySelectorAll('.feature-settings[data-feature-enable]').forEach(group=>{
  const parent=document.getElementById(group.dataset.featureParent),expected=group.dataset.featureEnable;
  const ready=featureEnabled(parent.id,expected),busy=sectionBusy.has(group.closest('details.card')?.id);
  group.classList.toggle('is-disabled',!ready);
  group.querySelectorAll('input,select,button:not(.help)').forEach(el=>{
   if(!el.hasAttribute('data-own-save-state')||!ready||busy)el.disabled=!ready||busy;
  });
  const note=group.querySelector('.feature-dependency'),key=parent.dataset.dirtyKey;
  if(note){
   note.hidden=ready;
   note.textContent=parent.dataset.savedLoaded!=='1'?'Loading settings…':
    String(fieldValue(parent))===expected?'Apply On to edit these settings.':
    String(confirmedValues[key])===expected?'Select On to edit, or apply Off to turn the function off.':'Turn on and apply to edit these settings.';
   if(parent.id==='noise_mode')note.textContent=note.textContent.replace('edit these settings.','edit settings and view the graph.');
  }
 });
 if(typeof updateNoiseSave==='function')updateNoiseSave();
}
for(const group of document.querySelectorAll('.feature-settings[data-feature-enable]'))
 document.getElementById(group.dataset.featureParent).addEventListener('change',updateFeatureDependencies);
updateFeatureDependencies();
