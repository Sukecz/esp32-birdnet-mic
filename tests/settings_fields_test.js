// Exercise the complete production settings module without slicing HTML functions.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const context=vm.createContext({confirmedValues:{},dirtyEls:{},edits:{},locks:{},editVersions:{},
 savingKeys:new Set(),pendingSyncKeys:new Set(),updateDiscard(){},Date});
vm.runInContext(fs.readFileSync('esp32-birdnet-mic/webui/src/js/settings/fields.js','utf8'),context);
function field(type,value){
 const listeners={},classes=new Set();
 return {type,value,checked:!!value,dataset:{},closest(){return null;},
  addEventListener(event,callback){listeners[event]=callback;},
  change(next){if(type==='checkbox')this.checked=next;else this.value=next;listeners.input();},
  classList:{toggle(name,on){if(on)classes.add(name);else classes.delete(name);},contains(name){return classes.has(name);}}};
}
for(const [type,initial,changed,equivalent] of [['number','1.50','2.75','1.5'],['text','Host','Other',' Host '],['checkbox',false,true,false],['password',' secret ','new',' secret ']]){
 const key=type,el=field(type,initial);context.trackEdit(el,key);
 context.syncInput(el,key,initial);el.change(changed);assert(el.classList.contains('dirty'));
 context.syncInput(el,key,initial);assert.equal(type==='checkbox'?el.checked:el.value,changed); // Poll preserves draft.
 el.change(equivalent);assert(!el.classList.contains('dirty'));assert(!context.edits[key]);
 el.change(changed);context.savingKeys.add(key);
 context.syncInput(el,key,equivalent);assert(el.classList.contains('dirty')); // In-flight poll cannot confirm a write.
 context.savingKeys.delete(key);context.clearDirty(key);context.syncInput(el,key,changed);
 assert(!el.classList.contains('dirty'));assert.equal(context.confirmedValues[key],context.fieldValue(el));
}
const number=field('number','1');context.trackEdit(number,'empty');number.change('');
assert(number.classList.contains('dirty'));context.syncInput(number,'empty','1');assert.equal(number.value,'');
const password=field('password','secret');context.trackEdit(password,'space');password.change(' secret ');
assert(password.classList.contains('dirty')); // Whitespace is significant for passwords.
console.log('Settings fields: normalization, edit/revert, polling, pending save, confirmation and blank/password drafts passed');
