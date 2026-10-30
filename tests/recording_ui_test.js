const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(require('node:path').join(__dirname,'../esp32-birdnet-mic/webui/index.html'),'utf8');
const script=html.match(/<script id='recording_script'>([\s\S]*?)<\/script>/)[1];
const ids=['recording_duration_mode','recording_minutes_label','recording_panel','device_mode','device_mode_status','device_mode_feedback','recording_summary','recording_file','recording_hours','recording_segment','recording_auto','recording_stop','recording_start','recording_save','recording_dirty','recording_feedback','recording_capacity'];
const nodes=Object.fromEntries(ids.map(id=>[id,{_value:'0',get value(){return this._value;},set value(v){this._value=String(v);},checked:false,textContent:'',disabled:false}]));
let requests=[],confirmResult=false,fail=false;
let j={recording:{mode:'recording',state:'ready',supported:true,busy:false,card_ready:false,elapsed_seconds:0,duration_minutes:360,segment_minutes:60,auto_start:false,written_kib:0,free_mib:1024,dropped_samples:0,file:'',error:''}};
const c=vm.createContext({wifiOffIntent:false,document:{documentElement:{dataset:{}},getElementById:id=>nodes[id]},URLSearchParams,AbortSignal,Math,Number,Error,confirm:()=>confirmResult,
 fetch:async(path,options)=>{assert.equal(options.headers['X-ESP32MIC-CSRF'],'1');requests.push({path,values:Object.fromEntries(options.body)});return{ok:!fail,json:async()=>({ok:!fail,error:'SD failure'})};},loadStatus:async()=>c.renderRecording(j)});
vm.runInContext(html.match(/function friendlyBytes\(n\)\{[^\n]+/)[0],c);vm.runInContext(script,c);
(async()=>{
 j.recording.duration_minutes=31;c.renderRecording(j);assert.equal(nodes.recording_dirty.textContent,'');assert(!nodes.recording_start.disabled);j.recording.duration_minutes=360;
 j.recording.state='checking';c.renderRecording(j);assert.equal(nodes.device_mode_status.textContent,'Recording: Checking SD…');
 j.recording.state='error';j.recording.error='SD unavailable; check card, power and wiring';c.renderRecording(j);assert.equal(nodes.device_mode_status.textContent,'Recording: Error — SD unavailable');
 j.recording.state='ready';j.recording.error='';j.recording.card_ready=true;
 c.renderRecording(j);assert.equal(nodes.recording_hours.value,'360');assert.equal(nodes.recording_panel.hidden,false);assert(nodes.recording_save.disabled);
 nodes.recording_hours.value='480';c.recordingDraftChanged();c.renderRecording(j);assert.equal(nodes.recording_hours.value,'480');assert.equal(nodes.recording_dirty.textContent,'Unsaved');assert(nodes.recording_start.disabled);
 nodes.recording_hours.value='360';c.recordingDraftChanged();assert.equal(nodes.recording_dirty.textContent,'');assert(nodes.recording_save.disabled);assert.equal(requests.length,0);
 j.recording.busy=true;j.recording.state='recording';j.recording.card_ready=true;c.renderRecording(j);
 assert.equal(nodes.device_mode_status.textContent,'Recording: OK');assert.equal(nodes.recording_start.disabled,true);assert.equal(nodes.recording_stop.disabled,false);
 assert.equal(nodes.recording_hours.disabled,true);assert.match(nodes.recording_summary.textContent,/1.1 GB free/);
 await c.changeDeviceMode('streaming');assert.equal(requests.length,0);assert.equal(nodes.device_mode.value,'recording');
 confirmResult=true;await c.changeDeviceMode('streaming');assert.deepEqual(requests.at(-1),{path:'/api/device/mode',values:{mode:'streaming',stop_confirmed:'1'}});
 j.recording.busy=false;j.recording.state='error';j.recording.error='SD full';c.renderRecording(j);assert.match(nodes.recording_summary.textContent,/SD full/);assert.equal(nodes.device_mode_status.textContent,'Recording: Error — SD full');
 fail=true;await c.recordingAction('stop');assert.equal(nodes.recording_feedback.textContent,'SD failure');
 j.recording.error='';j.recording.state='cooling';j.recording.cooling_pending=true;j.recording.space_seconds=7200;c.renderRecording(j);
 assert.equal(nodes.device_mode_status.textContent,'Recording: Paused — cooling down');assert(!nodes.recording_stop.disabled);assert(nodes.recording_start.disabled);assert(nodes.recording_hours.disabled);
 assert.match(nodes.recording_capacity.textContent,/approximately 2h/);assert.match(nodes.recording_capacity.textContent,/less than/);
 j.recording.space_seconds=null;c.renderRecording(j);assert.match(nodes.recording_capacity.textContent,/unavailable/);
 j.recording.cooling_pending=false;
 fail=false;j.recording.mode='streaming';c.renderRecording(j);assert(nodes.recording_panel.hidden);
 // Listen remains available: renderRecording must not alter or disable its button.
 assert(!script.includes("recordingNode('listen_button')"));
 console.log('Recording UI: polling/drafts, restored values, busy controls, confirmation and error feedback passed');
})().catch(e=>{console.error(e);process.exitCode=1});
