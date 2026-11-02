function fieldValue(el,value=el.type==='checkbox'?el.checked:el.value){
 if(el.type==='checkbox')return !!value;
 const text=String(value??'');
 if(el.type==='number'&&text.trim()!==''&&Number.isFinite(Number(text.replace(',','.'))))return Number(text.replace(',','.'));
 return el.type==='password'?text:text.trim();
}
function differsFromSaved(key){
 return (dirtyEls[key]||[]).some(el=>fieldValue(el)!==confirmedValues[key]);
}
function reconcileEdit(key){
 if(differsFromSaved(key)||pendingSyncKeys.has(key))edits[key]=Infinity;
 else{delete edits[key];delete locks[key];}
 (dirtyEls[key]||[]).forEach(el=>toggleDirty(el,key));
}
function syncInput(el,key,value){
 if(!el)return;
 if(savingKeys.has(key)){toggleDirty(el,key);return;}
 const editing=!!edits[key],locked=locks[key]&&Date.now()<locks[key];
 confirmedValues[key]=fieldValue(el,value);pendingSyncKeys.delete(key);
 if(locked&&!editing){toggleDirty(el,key);return;}
 if(!editing){if(el.type==='checkbox')el.checked=!!value;else el.value=value;}
 reconcileEdit(key);
}
function trackEdit(el,key){
 if(!el)return; el.dataset.dirtyKey=key;
 if(!dirtyEls[key]) dirtyEls[key]=[];
 if(!dirtyEls[key].includes(el)) dirtyEls[key].push(el);
 if(!Object.hasOwn(confirmedValues,key))confirmedValues[key]=fieldValue(el);
 const bump=()=>{editVersions[key]=(editVersions[key]||0)+1;reconcileEdit(key);};
 el.addEventListener('input',bump);el.addEventListener('change',bump);
}
function markDirty(key){edits[key]=Infinity; (dirtyEls[key]||[]).forEach(el=>toggleDirty(el,key));}
function clearDirty(key){delete edits[key];delete locks[key];(dirtyEls[key]||[]).forEach(el=>toggleDirty(el,key));}
function toggleDirty(el,key){
 if(!el)return; const dirty=!!edits[key]&&differsFromSaved(key); el.classList.toggle('dirty',dirty);
 const field=el.closest('.field');
 if(field){let note=field.querySelector('.edit-note');if(!note){note=document.createElement('span');note.className='edit-note';field.append(note);}note.textContent=dirty?'Unsaved':'';}
 updateDiscard(el.closest('details.card'));
}
