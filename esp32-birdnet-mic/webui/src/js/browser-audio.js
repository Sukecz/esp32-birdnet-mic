
let listenSession=null;
function listenMessage(text){ document.getElementById('listen_status').textContent=text; }
function setListenVolume(value){
 const percent=Math.min(150,Math.max(0,Number(value)||0));
 document.getElementById('listen_volume_value').textContent=percent+'%';
 if(listenSession) listenSession.gain.gain.value=percent/100;
}
function clearListenAudio(session){
 for(const source of session.sources){try{source.stop();}catch(_){}}
 session.sources.clear();session.next=0;
}
function queueListenAudio(session,bytes,rate){
 const {context,gain}=session,now=context.currentTime;
 // Schedule by PCM sample count, never by fetch packet arrival. TCP can merge
 // many capture blocks or pause briefly while the browser services the UI.
 // Keep a playback reserve on the browser, without using ESP capture RAM.
 const maxSeconds=2,maxSamples=Math.floor(rate*maxSeconds);
 if(session.next>now+maxSeconds){
  session.stats.resyncs++;clearListenAudio(session);
 }
 let offset=0;
 if(bytes.length/2>maxSamples){offset=bytes.length-maxSamples*2;session.stats.droppedSamples+=offset/2;}
 if(!session.next||session.next<now+0.015){
  if(session.next){session.stats.underruns++;session.reserve=Math.min(1.5,session.reserve+0.25);}
  session.next=now+session.reserve;
 }
 // Preserve every sample in ordinary bursts, with bounded nodes and memory.
 const blockSamples=Math.max(1,Math.floor(rate*0.1));
 while(offset<bytes.length){
  const count=Math.min((bytes.length-offset)/2,blockSamples);
  const buffer=context.createBuffer(1,count,rate),out=buffer.getChannelData(0);
  const pcm=new DataView(bytes.buffer,bytes.byteOffset+offset,count*2);
  for(let i=0;i<count;i++)out[i]=pcm.getInt16(i*2,true)/32768;
  const source=context.createBufferSource();source.buffer=buffer;source.connect(gain);
  session.sources.add(source);source.onended=()=>{session.sources.delete(source);source.disconnect();};
  source.start(session.next);session.next+=buffer.duration;offset+=count*2;
 }
}
function stopListening(message='Listening stopped.'){
 const session=listenSession;listenSession=null;
 if(session){session.abort.abort();session.request?.abort();clearListenAudio(session);session.context.close().catch(()=>{});}
 const button=document.getElementById('listen_button');
 button.textContent='Listen';button.setAttribute('aria-pressed','false');listenMessage(message);
}
function listenDelay(session,ms){
 return new Promise(resolve=>{
  const finish=()=>{clearTimeout(timer);session.abort.signal.removeEventListener('abort',finish);resolve();};
  const timer=setTimeout(finish,ms);
  session.abort.signal.addEventListener('abort',finish,{once:true});
  if(session.abort.signal.aborted)finish();
 });
}
function listenWait(promise,session,request){
 return new Promise((resolve,reject)=>{
  let finished=false;
  const done=(fn,value)=>{if(finished)return;finished=true;clearTimeout(timer);session.abort.signal.removeEventListener('abort',cancel);fn(value);};
  const cancel=()=>{const error=new Error('Listening stopped.');error.name='AbortError';done(reject,error);request.abort();};
  const timer=setTimeout(()=>{const error=new Error('Live audio stalled.');error.retryable=true;done(reject,error);request.abort();},4000);
  session.abort.signal.addEventListener('abort',cancel,{once:true});
  Promise.resolve(promise).then(value=>done(resolve,value),error=>done(reject,error));
  if(session.abort.signal.aborted)cancel();
 });
}
async function toggleListen(){
 if(listenSession){stopListening();return;}
 const Audio=window.AudioContext||window.webkitAudioContext;
 if(!Audio){listenMessage('Audio playback is unavailable in this browser.');return;}
 const context=new Audio(), gain=context.createGain();
 gain.gain.value=Number(document.getElementById('listen_volume').value)/100; gain.connect(context.destination);
 const session={context,gain,abort:new AbortController(),request:null,sources:new Set(),next:0,reserve:0.5,stats:{underruns:0,resyncs:0,droppedSamples:0}};
 listenSession=session;
 const button=document.getElementById('listen_button');
 button.textContent='Stop listening'; button.setAttribute('aria-pressed','true');
 listenMessage('Connecting…');
 try{await context.resume();}catch(_){if(listenSession===session)stopListening('Could not start audio playback.');return;}
 const url=new URL(location.href);url.port='81';url.pathname='/listen.wav';url.search='';url.hash='';
 let failures=0;
 while(listenSession===session){
  const request=new AbortController();session.request=request;
  try{
   let response;
   for(let attempt=0;attempt<4;attempt++){
     response=await listenWait(fetch(url.href,{cache:'no-store',signal:request.signal}),session,request);
     if(listenSession!==session)return;
     if(response.status!==503||attempt===3)break;
     listenMessage('Waiting for Listen…');await listenDelay(session,250);
     if(listenSession!==session)return;
   }
   if(!response.ok)throw new Error(response.status===503?'Listening is busy or streaming is paused. Stop other listening tabs and retry.':'Could not open live audio.');
   const reader=response.body.getReader();
   let header=new Uint8Array(44),headerBytes=0,rate=0,carry=null,healthySince=0;
   while(listenSession===session){
     const {done,value}=await listenWait(reader.read(),session,request);
     if(done){const error=new Error('Live audio connection ended.');error.retryable=true;throw error;}
     let bytes=value;
     if(headerBytes<44){
       const take=Math.min(44-headerBytes,bytes.length);header.set(bytes.subarray(0,take),headerBytes);headerBytes+=take;bytes=bytes.subarray(take);
       if(headerBytes<44)continue;
       const view=new DataView(header.buffer);rate=view.getUint32(24,true);
       if(String.fromCharCode(...header.subarray(0,4))!=='RIFF'||view.getUint16(20,true)!==1||view.getUint16(22,true)!==1||view.getUint16(34,true)!==16||rate<8000||rate>192000)
         throw new Error('Unsupported audio stream.');
       listenMessage('Listening');
     }
     if(carry!==null){const joined=new Uint8Array(bytes.length+1);joined[0]=carry;joined.set(bytes,1);bytes=joined;carry=null;}
     if(bytes.length%2){carry=bytes[bytes.length-1];bytes=bytes.subarray(0,bytes.length-1);}
     if(!bytes.length)continue;
     if(!healthySince)healthySince=Date.now();
     if(Date.now()-healthySince>=10000)failures=0;
     queueListenAudio(session,bytes,rate);
   }
  }catch(error){
   request.abort();
   if(listenSession!==session)return;
   if(typeof wifiOffIntent!=='undefined'&&wifiOffIntent){stopListening('Listening stopped: device Wi-Fi is off.');return;}
   if(!(error.retryable||error.name==='TypeError')||++failures>3){
    stopListening(error.retryable||error.name==='TypeError'?'Live audio connection failed. Press Listen to retry.':(error.message||'Could not play audio.'));return;
   }
   clearListenAudio(session);listenMessage('Reconnecting…');await listenDelay(session,500);
  }finally{request.abort();if(session.request===request)session.request=null;}
 }

}
window.addEventListener('pagehide',()=>stopListening());
document.addEventListener('visibilitychange',()=>{if(document.hidden&&listenSession)stopListening();});
