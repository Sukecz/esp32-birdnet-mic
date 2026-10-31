const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../esp32-birdnet-mic/webui/index.html'),'utf8');
const script=html.match(/<script id='browser_audio_script'>([\s\S]*?)<\/script>/)[1];
let now=1000,nextTimer=0,calls=0,mode='normal',rate=48000;const timers=new Map(),streams=[],contexts=[],buffers=[],sources=[];
const nodes=Object.fromEntries(['listen_button','listen_volume','listen_volume_value','listen_status'].map(id=>[id,{textContent:'',value:'125',setAttribute(k,v){this[k]=v;}}]));
class AudioContext{
 constructor(){contexts.push(this);this.state='running';}get currentTime(){return now/1000;}
 createGain(){return{gain:{value:0},connect(){}};}resume(){return Promise.resolve();}close(){this.closed=true;return Promise.resolve();}
 createBuffer(channels,count,sampleRate){const data=new Float32Array(count),b={duration:count/sampleRate,rate:sampleRate,data,getChannelData:()=>data};buffers.push(b);return b;}
 createBufferSource(){const s={connect(){},disconnect(){},start(){},stop(){this.stopped=true;}};sources.push(s);return s;}
}
function wave(hz){const b=new Uint8Array(49),v=new DataView(b.buffer);b.set(Buffer.from('RIFF'));v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,hz,true);v.setUint16(34,16,true);v.setInt16(44,1234,true);v.setInt16(46,-2345,true);b[48]=0x7f;return b;}
const c=vm.createContext({window:{AudioContext,addEventListener(){}},document:{getElementById:id=>nodes[id],addEventListener(){}},location:{href:'http://mic.local/'},URL,AbortController,DataView,Uint8Array,Set,Math,Number,Error,Date:{now:()=>now},wifiOffIntent:false,
 setTimeout:(fn,ms)=>{const id=++nextTimer;timers.set(id,{fn,at:now+ms});return id;},clearTimeout:id=>timers.delete(id),
 fetch:async(url,options)=>{
  ++calls;if(mode==='fail')throw new TypeError('Failed to fetch');
  const channel={sent:false,pending:null,abort:false};streams.push(channel);
  options.signal.addEventListener('abort',()=>{channel.abort=true;if(channel.pending)channel.pending.reject(Object.assign(new Error('Aborted'),{name:'AbortError'}));},{once:true});
  return{ok:true,status:200,body:{getReader:()=>({read:()=>{
   if(!channel.sent){channel.sent=true;return Promise.resolve({done:false,value:wave(rate)});}
   return new Promise((resolve,reject)=>channel.pending={resolve,reject});
  }})}};
 }});
vm.runInContext(script,c);const tick=()=>new Promise(r=>setImmediate(r));
async function advance(ms){const until=now+ms;for(;;){const first=[...timers].filter(([,v])=>v.at<=until).sort((a,b)=>a[1].at-b[1].at)[0];if(!first)break;now=first[1].at;timers.delete(first[0]);first[1].fn();await tick();}now=until;await tick();}
const active=()=>vm.runInContext('!!listenSession',c);
(async()=>{
 const playing=c.toggleListen();await tick();assert.equal(nodes.listen_status.textContent,'Listening');
 const originalContext=contexts.at(-1);assert.equal(vm.runInContext('listenSession.gain.gain.value',c),1.25);
 // EOF must reconnect, clear old scheduled audio/odd-byte carry, and parse a new rate.
 streams.at(-1).pending.resolve({done:true});await tick();assert.equal(nodes.listen_status.textContent,'Reconnecting…');
 assert(!originalContext.closed&&sources.every(s=>s.stopped));rate=16000;await advance(500);
 assert.equal(nodes.listen_status.textContent,'Listening');assert.equal(contexts.length,1);assert.equal(buffers.at(-1).rate,16000);
 assert.deepEqual(Array.from(buffers.at(-1).data),[1234/32768,-2345/32768]);assert.equal(vm.runInContext('listenSession.gain.gain.value',c),1.25);
 // A read that never finishes has a deadline; its old TCP request is aborted.
 await advance(4000);assert.equal(nodes.listen_status.textContent,'Reconnecting…');assert(streams.at(-1).abort);
 rate=44100;await advance(500);assert.equal(nodes.listen_status.textContent,'Listening');assert.equal(buffers.at(-1).rate,44100);
 // Manual Stop during recovery cancels the pending retry immediately.
 streams.at(-1).pending.resolve({done:true});await tick();const previous=calls;c.stopListening();await playing;await advance(1000);
 assert.equal(calls,previous);assert(!active()&&originalContext.closed);assert.equal(timers.size,0);
 // Repeated connection failures are bounded; do not reconnect forever.
 mode='fail';const before=calls;const failing=c.toggleListen();await tick();await advance(1600);await failing;
 assert.equal(calls-before,4);assert(!active());assert.match(nodes.listen_status.textContent,/Press Listen to retry/);assert.equal(timers.size,0);
 // Intentional Wi-Fi-off must not trigger reconnection attempts.
 mode='normal';rate=48000;const offline=c.toggleListen();await tick();c.wifiOffIntent=true;
 const offlineCalls=calls;streams.at(-1).pending.resolve({done:true});await offline;
 assert(!active());assert.equal(calls,offlineCalls);assert.match(nodes.listen_status.textContent,/device Wi-Fi is off/);assert.equal(timers.size,0);
 console.log('Listen recovery: EOF, stalled reads, fresh WAV/rate and carry, volume/context preservation, bounded failures, Stop and intentional Wi-Fi-off passed.');
})().catch(e=>{console.error(e);process.exitCode=1;});
