// Deterministic PCM playback timeline: network jitter must not create gaps or
// discard samples, and a real underrun must rebuild a bounded playback reserve.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const script=fs.readFileSync('esp32-birdnet-mic/webui/index.html','utf8').match(/<script id='browser_audio_script'>([\s\S]*?)<\/script>/)[1];
const sandbox=vm.createContext({window:{addEventListener(){}},document:{addEventListener(){}},DataView,Uint8Array});
vm.runInContext(script,sandbox);
function fixture(){
 const sources=[],context={currentTime:0,
  createBuffer(channels,count,rate){const data=new Float32Array(count);return{duration:count/rate,getChannelData:()=>data,data};},
  createBufferSource(){const source={connect(){},disconnect(){},stop(){this.stopped=true;},start(at){this.at=at;}};sources.push(source);return source;}};
 return{context,gain:{},sources:new Set(),next:0,reserve:0.5,stats:{underruns:0,resyncs:0,droppedSamples:0},scheduled:sources};
}
function pcm(count,start=0){const bytes=new Uint8Array(count*2),v=new DataView(bytes.buffer);for(let i=0;i<count;i++)v.setInt16(i*2,(start+i)%30000,true);return bytes;}
function continuous(sources){for(let i=1;i<sources.length;i++)assert(Math.abs(sources[i].at-sources[i-1].at-sources[i-1].buffer.duration)<1e-9,'Inserted silence between PCM blocks');}
for(const rate of [8000,44100,48000,192000]){
 const s=fixture(),block=Math.floor(rate/100);
 // 10ms blocks arrive unevenly; low reserve does not mean an actual underrun.
 for(const now of [0,0.01,0.03,0.13,0.2,0.35,0.4]){s.context.currentTime=now;sandbox.queueListenAudio(s,pcm(block),rate);}
 continuous(s.scheduled);assert.equal(s.stats.underruns,0);
 // A coalesced 600ms HTTP read must preserve its beginning, middle and end.
 const bytes=pcm(Math.floor(rate*0.6),300),first=s.scheduled.length;
 sandbox.queueListenAudio(s,bytes,rate);continuous(s.scheduled);
 const actual=s.scheduled.slice(first).flatMap(source=>Array.from(source.buffer.data));
 assert.equal(actual.length,bytes.length/2);const view=new DataView(bytes.buffer);
 actual.forEach((v,i)=>assert.equal(v,view.getInt16(i*2,true)/32768));
 assert.equal(s.stats.droppedSamples,0);
 // A genuine outage rebuilds the reserve once, then resumes continuously.
 s.context.currentTime=s.next+0.1;sandbox.queueListenAudio(s,pcm(block),rate);
 assert.equal(s.stats.underruns,1);assert.equal(s.reserve,0.75);
 assert.equal(s.scheduled.at(-1).at,s.context.currentTime+0.75);
 s.context.currentTime+=0.01;const previous=s.next;sandbox.queueListenAudio(s,pcm(block),rate);
 assert.equal(s.scheduled.at(-1).at,previous);assert.equal(s.stats.underruns,1);
 // Repeated stalls cap the reserve and an excessive backlog releases old nodes.
 for(let i=0;i<10;i++){s.context.currentTime=s.next+1;sandbox.queueListenAudio(s,pcm(block),rate);}
 assert.equal(s.reserve,1.5);
 s.next=s.context.currentTime+3;sandbox.queueListenAudio(s,pcm(block),rate);
 assert.equal(s.stats.resyncs,1);assert(s.scheduled.slice(0,-1).every(source=>source.stopped));
 // Even an anomalously huge read cannot allocate unbounded audio buffers.
 const huge=fixture();sandbox.queueListenAudio(huge,pcm(rate*5),rate);
 assert.equal(huge.stats.droppedSamples,rate*3);assert(huge.next<=2.500001);assert(huge.sources.size<=21);
}
console.log('Listen buffering: gapless jitter/bursts, exact PCM, underrun recovery, adaptive reserve and bounded backlog passed at 8/44.1/48/192 kHz.');
