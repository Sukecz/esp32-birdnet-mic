
document.addEventListener('DOMContentLoaded',()=>{
 window.deviceApPassword=window.createApPasswordEditor({
  saved:password=>{document.getElementById('direct_access_password').textContent=password;directAccessLoaded=true;},
  changed:()=>updateDiscard(document.getElementById('networksec')),
  locked:()=>!!recordingState?.busy||!!recordingState?.cooling_pending
 });
 loadDirectAccess();
});
