'use strict';
// The same small editor is used in Network and Device configuration.
window.createApPasswordEditor=function(options={}){
 const n=id=>document.getElementById('ap_password_'+id);
 let saved=null,opened=false,pending=false;
 const dirty=()=>opened&&saved!==null&&n('input').value!==saved;
 function feedback(text,error=false){n('feedback').textContent=text;n('feedback').classList.toggle('error',error);}
 function render(){
  const changed=dirty();
  n('editor').hidden=!opened;n('change').hidden=opened;
  n('change').setAttribute('aria-expanded',String(opened));
  n('input').classList.toggle('dirty',changed);n('dirty').textContent=changed?'Unsaved':'';
  n('save').disabled=pending||!changed;n('cancel').disabled=pending;
  n('change').disabled=pending;n('input').disabled=pending;n('show').disabled=pending;
  options.changed?.();
 }
 function confirmed(password){
  if(typeof password!=='string')return;
  const preserve=dirty();saved=password;
  if(!preserve)n('input').value=saved;
  render();
 }
 async function request(values){
  const response=await fetch('/api/setup/access',{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8','X-ESP32MIC-CSRF':'1'},body:new URLSearchParams(values),signal:AbortSignal.timeout(10000)});
  const result=await response.json();
  if(!response.ok||!result.ok)throw new Error(result.error||'Could not save the password. Try again.');
  if(typeof result.password!=='string')throw new Error('Could not confirm the password. Reopen this page to check.');
  return result.password;
 }
 function cancel(){
  if(pending)return;
  opened=false;n('input').value=saved||'';n('input').type='password';
  n('show').textContent='Show';n('show').setAttribute('aria-pressed','false');feedback('');render();
 }
 async function save(){
  if(pending||!dirty())return;
  const password=n('input').value;
  if(!/^[\x20-\x7e]{8,63}$/.test(password)){feedback('Use 8–63 characters: letters without accents, numbers, spaces or symbols.',true);return;}
  if(options.locked?.()){feedback('Stop recording and finish other changes before saving the password.',true);return;}
  pending=true;render();feedback('Saving…');
  try{
   const passwordSaved=await request({password});
   saved=passwordSaved;options.saved?.(saved);
   if(n('input').value===password){opened=false;n('input').value=saved;n('input').type='password';n('show').textContent='Show';n('show').setAttribute('aria-pressed','false');}
   feedback('Password saved. Use it the next time you join device Wi-Fi. If disconnected, reconnect with the new password.');
  }catch(error){feedback(error.message==='Failed to fetch'?'Connection interrupted. Your change is still here. Reconnect and try again.':error.message,true);}
  finally{pending=false;render();}
 }
 n('change').onclick=async()=>{
  if(pending)return;pending=true;render();feedback('');
  try{confirmed(await request({}));opened=true;}
  catch(_){feedback('Could not load the current password. Try again.',true);}
  finally{pending=false;render();if(opened)n('input').focus();}
 };
 n('input').addEventListener('input',render);
 n('input').addEventListener('keydown',event=>{if(event.key==='Enter'){event.preventDefault();event.stopPropagation();save();}});
 n('show').onclick=()=>{const show=n('input').type==='password';n('input').type=show?'text':'password';n('show').textContent=show?'Hide':'Show';n('show').setAttribute('aria-pressed',String(show));};
 n('cancel').onclick=cancel;n('save').onclick=save;
 window.addEventListener('beforeunload',event=>{if(dirty()||pending){event.preventDefault();event.returnValue='You have unsaved changes.';}});
 render();return {confirmed,cancel,dirty,busy:()=>pending};
};
