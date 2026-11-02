// Test complete production settings fragments, including overlapping edits/saves.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const context=vm.createContext({dirtyEls:{},edits:{},locks:{},pollPromise:null,
 $(){return {};},updateDiscard(){},Date,URLSearchParams});
for(const name of ['groups','fields'])vm.runInContext(fs.readFileSync(`esp32-birdnet-mic/webui/src/js/settings/${name}.js`,'utf8'),context);
context.feedback=()=>{};context.updateDiscard=()=>{};
function field(value){
 const listeners={},classes=new Set();
 return {type:'number',value,dataset:{},closest(){return null;},
  addEventListener(event,callback){listeners[event]=callback;},
  change(value){this.value=value;listeners.input();},
  classList:{toggle(name,on){if(on)classes.add(name);else classes.delete(name);},contains(name){return classes.has(name);}}};
}
(async()=>{
 const gain=field('1.5'),shift=field('12');
 context.trackEdit(gain,'gain');context.trackEdit(shift,'shift');
 gain.change('2');shift.change('13');let writes=0,saved='2',release;
 context.refreshAll=async()=>context.syncInput(gain,'gain',saved);
 context.mutationFetch=async()=>{writes++;return {ok:true,json:async()=>({ok:true})};};
 assert(await context.saveValues('/api/set',{value:'2'},['gain'],'audio'));
 assert(!gain.classList.contains('dirty'));assert(shift.classList.contains('dirty')); // Scope is one group.
 gain.change('3');context.mutationFetch=async()=>{throw Error('offline');};
 assert(!await context.saveValues('/api/set',{value:'3'},['gain'],'audio'));
 assert.equal(gain.value,'3');assert(gain.classList.contains('dirty')); // Failed save retains draft.
 context.mutationFetch=()=>new Promise(resolve=>release=()=>resolve({ok:true,json:async()=>({ok:true})}));
 const save=context.saveValues('/api/set',{value:'3'},['gain'],'audio');
 assert(!await context.saveValues('/api/set',{value:'3'},['gain'],'audio')); // No overlapping write.
 gain.change('4');saved='3';release();assert(await save);
 assert.equal(gain.value,'4');assert(gain.classList.contains('dirty')); // Newer edit survives confirmation.
 let finishPoll;context.pollPromise=new Promise(resolve=>finishPoll=resolve);
 context.mutationFetch=async()=>({ok:true,json:async()=>({ok:true})});
 const last=context.saveValues('/api/set',{value:'4'},['gain'],'audio');
 await Promise.resolve();context.syncInput(gain,'gain','1.5');assert.equal(gain.value,'4');
 saved='4';finishPoll();assert(await last);assert(!gain.classList.contains('dirty'));
 assert(shift.classList.contains('dirty'));assert.equal(writes,1);
 console.log('Settings groups: scoped confirmation, failed saves, duplicate saves, newer edits and stale polling passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
