const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../esp32-birdnet-mic/webui/index.html'),'utf8');
const script=html.match(/<script id='browser_audio_script'>([\s\S]*?)<\/script>/)[1];
const nodes=Object.fromEntries(['listen_button','listen_volume','listen_volume_value','listen_status','mic_signal_status'].map(id=>[id,{textContent:'',value:'60',setAttribute(key,value){this[key]=value;}}]));
const events={},buffers=[],sources=[],contexts=[];
let reads=[],pendingRead,fetchStatus=200;
class AudioContext {
 constructor(){this.currentTime=0;contexts.push(this);}
 createGain(){return {gain:{value:0},connect(){}};}
 resume(){return Promise.resolve();}
 close(){this.closed=true;return Promise.resolve();}
 createBuffer(channels,count,rate){const data=new Float32Array(count);const b={channels,count,rate,duration:count/rate,data,getChannelData:()=>data};buffers.push(b);return b;}
 createBufferSource(){const s={connect(){},disconnect(){},start(at){this.at=at;},stop(){this.stopped=true;}};sources.push(s);return s;}
}
const c=vm.createContext({window:{AudioContext,addEventListener:(name,fn)=>events[name]=fn},document:{hidden:false,getElementById:id=>nodes[id],addEventListener:(name,fn)=>events[name]=fn},
 location:{href:'http://mic.local/?test=1#x'},URL,AbortController,DataView,Uint8Array,Set,Math,Number,Error,
 fetch:async(url,options)=>{
   assert.equal(url,'http://mic.local:81/listen.wav');assert.equal(options.cache,'no-store');
   return {ok:fetchStatus===200,status:fetchStatus,body:{getReader:()=>({read:()=>reads.length?Promise.resolve(reads.shift()):new Promise(resolve=>pendingRead=resolve)})}};
 }});
vm.runInContext(script,c);
const tick=()=>new Promise(resolve=>setImmediate(resolve));
function stream(rate,pcm){
 const header=new Uint8Array(44),view=new DataView(header.buffer);
 header.set(Buffer.from('RIFF'));view.setUint16(20,1,true);view.setUint16(22,1,true);view.setUint32(24,rate,true);view.setUint16(34,16,true);
 const bytes=new Uint8Array(44+pcm.length*2);bytes.set(header);
 pcm.forEach((sample,i)=>new DataView(bytes.buffer).setInt16(44+i*2,sample,true));
 // Header and sample words split across three odd-sized network reads.
 reads=[bytes.subarray(0,13),bytes.subarray(13,45),bytes.subarray(45)].map(value=>({done:false,value}));
}
(async()=>{
 for(const rate of [8000,48000,192000]){
   const initial=buffers.length;stream(rate,[-32768,-1,0,1,32767]);
   const playing=c.toggleListen();await tick();
   assert.equal(nodes.listen_status.textContent,'Listening');assert.equal(nodes.listen_button['aria-pressed'],'true');
   assert(buffers.slice(initial).every(b=>b.rate===rate&&b.channels===1));
   assert.deepEqual(buffers.slice(initial).flatMap(b=>Array.from(b.data)),[-1,-1/32768,0,1/32768,32767/32768]);
   c.setListenVolume('20');assert.equal(vm.runInContext('listenSession.gain.gain.value',c),0.2);
   c.setListenVolume('100');assert.equal(vm.runInContext('listenSession.gain.gain.value',c),1);assert.equal(nodes.listen_volume_value.textContent,'100%');
   c.setListenVolume('150');assert.equal(vm.runInContext('listenSession.gain.gain.value',c),1.5);assert.equal(nodes.listen_volume_value.textContent,'150%');
   c.setListenVolume('200');assert.equal(vm.runInContext('listenSession.gain.gain.value',c),1.5);
   c.setListenVolume('0');assert.equal(vm.runInContext('listenSession.gain.gain.value',c),0);
   c.stopListening();pendingRead({done:true});await playing;
   assert.equal(nodes.listen_button.textContent,'Listen');assert(contexts.at(-1).closed);
 }
 fetchStatus=503;await c.toggleListen();assert.match(nodes.listen_status.textContent,/busy or streaming is paused/);
 fetchStatus=200;stream(48000,[10,20]);const first=c.toggleListen();await tick();const oldRead=pendingRead;
 c.stopListening();stream(48000,[30,40]);const second=c.toggleListen();await tick();
 oldRead({done:true});await first;assert.equal(nodes.listen_status.textContent,'Listening'); // Old completion cannot close new session.
 c.document.hidden=true;events.visibilitychange();pendingRead({done:true});await second;
 assert.equal(nodes.listen_button.textContent,'Listen');assert(sources.every(source=>source.stopped));
 console.log('Browser audio: split headers/PCM, I2S/PDM rate handling, volume, busy, cancellation, reconnect and hidden-page cleanup passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
