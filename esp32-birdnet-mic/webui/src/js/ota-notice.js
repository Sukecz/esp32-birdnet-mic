
(function(){
  const notice=document.getElementById('ota_notice');
  const link=document.getElementById('ota_link');
  let tries=0;
  function check(){
    fetch('/api/ota_status',{cache:'no-store'})
      .then(r=>r.json())
      .then(j=>{
      if(j&&j.checking){
        link.title='Checking for firmware updates in the background…';
        if(tries++<10)setTimeout(check,750);
        return;
      }
      if(j&&j.update_available){
        const version=String(j.latest_version||'').trim();
        notice.textContent='New firmware v'+version+' is available — open Firmware update';
        notice.classList.add('show');
        link.classList.add('ota-ready');
        link.textContent='Update available: v'+version;
      }else if(j&&!j.ok){
        link.title='Update check unavailable. You can still upload firmware manually.';
      }
      })
      .catch(()=>{link.title='Update check unavailable. You can still upload firmware manually.';});
  }
  check();
})();
