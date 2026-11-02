
(function(){
  const help=document.getElementById('h_mic_format');
  const hint=document.getElementById('row_mic_format_hint');
  if(help&&hint){ help.setAttribute('aria-controls',hint.id); help.onclick=()=>{const open=hint.style.display==='none'; hint.style.display=open?'':'none'; help.setAttribute('aria-expanded',String(open));}; }
  trackEdit(document.getElementById('sel_s3_board'),'s3_board');
  trackEdit(document.getElementById('sel_mic_format'),'mic_format');document.getElementById('sel_mic_format').addEventListener('change',()=>{document.getElementById('mic_format_label').textContent=document.getElementById('sel_mic_format').selectedOptions[0]?.textContent||'';});
})();
