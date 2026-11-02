const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../esp32-birdnet-mic/webui/index.html'),'utf8');
const ids=['sel_mic_format','opt_pdm','in_rate','btn_rate_set','in_shift','btn_shift_set','txt_gain_hint','mic_format_label','mic_detection_note','btn_mic_recommended'];
const nodes=Object.fromEntries(ids.map(id=>[id,{value:'0'}]));
nodes.sel_mic_format.selectedOptions=[{textContent:'Microphone model'}];
const edits={},locks={};
const c=vm.createContext({$:id=>nodes[id],document:{getElementById:id=>nodes[id]},Date,edits,locks,confirmedValues:{},dirtyEls:{mic_format:[nodes.sel_mic_format]},savingKeys:new Set(),pendingSyncKeys:new Set(),
 T:{en:{help_gain:'Original I2S gain help'}},lang:'en',toggleDirty:()=>{}});
vm.runInContext(html.slice(html.indexOf('function fieldValue('),html.indexOf('function trackEdit('))+html.slice(html.indexOf('function clearDirty('),html.indexOf('function toggleDirty(')),c);
const a=html.indexOf('function renderMicFormat('),b=html.indexOf('function loadAudio(',a);
vm.runInContext(fs.readFileSync(path.join(__dirname,'../esp32-birdnet-mic/webui/src/js/detection-display.js'),'utf8')+html.slice(a,b),c);
c.renderMicFormat({mic_format:2,pdm_supported:true});
assert.equal(nodes.sel_mic_format.value,'2');
for(const id of ['in_rate','btn_rate_set','in_shift','btn_shift_set']) assert.equal(nodes[id].disabled,true);
assert(!nodes.txt_gain_hint.textContent.includes('raise I2S Shift'));
assert.equal(nodes.opt_pdm.disabled,false);
// A stale poll does not overwrite a user's unsaved selection.
nodes.sel_mic_format.value='1'; edits.mic_format=Date.now()+10000;
c.renderMicFormat({mic_format:2,pdm_supported:true});
assert.equal(nodes.sel_mic_format.value,'1');
delete edits.mic_format; locks.mic_format=Date.now()+3000;
c.renderMicFormat({mic_format:2,pdm_supported:true});
assert.equal(nodes.sel_mic_format.value,'1');
delete locks.mic_format;
c.renderMicFormat({mic_format:0,pdm_supported:false});
assert.equal(nodes.sel_mic_format.value,'0');
for(const id of ['in_rate','btn_rate_set','in_shift','btn_shift_set']) assert.equal(nodes[id].disabled,false);
assert.equal(nodes.opt_pdm.disabled,true);
assert.equal(nodes.txt_gain_hint.textContent,'Original I2S gain help');
assert(html.includes('Auto checks supported S3 boards'));
console.log('PDM UI mode, capability, controls, help and edit locks passed');

// Constrained boards must not be advised to increase to 1024/2048 samples.
nodes.adv={textContent:''};
c.$=id=>nodes[id];
c.T.en.adv_buf512='Consider 512';c.T.en.adv_buf1024='Consider 1024';
vm.runInContext(html.slice(html.indexOf('function updateAdvice('),html.indexOf('function loadPerf(')),c);
for(const size of [256,1024,2048]){
 c.updateAdvice({buffer_size:size,buffer_size_recommended:512,gain:1,mic_format:2});
 assert(nodes.adv.textContent.includes('Use 512 samples'));
 assert(!nodes.adv.textContent.includes('Consider 1024'));
}
c.updateAdvice({buffer_size:512,buffer_size_recommended:512,gain:1});
assert.equal(nodes.adv.textContent,'');
c.updateAdvice({buffer_size:512,buffer_size_recommended:0,gain:1});
assert(nodes.adv.textContent.includes('Consider 1024'));
console.log('C3/C5 packet size advice passed');
