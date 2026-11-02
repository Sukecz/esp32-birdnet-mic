
fetch('/api/update_space',{cache:'no-store'}).then(r=>r.ok?r.json():null).then(s=>{
  if(s&&['available','error','working','recovery_required','check_failed'].includes(s.state))document.getElementById('update_space_notice').hidden=false;
}).catch(()=>{});
