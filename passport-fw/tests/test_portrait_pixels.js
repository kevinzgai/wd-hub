const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('main/apps/home/phone_setup.html','utf8');
const algorithm=html.slice(html.indexOf('function portraitLocalContrast('),html.indexOf('async function imageFile('));
const elements={portraitBackground:{checked:false},portraitBrightness:{value:'0'},portraitPresetDot:{},portraitPresetFlat:{},portraitPresetHybrid:{}};
const context=vm.createContext({$:id=>elements[id],PORTRAIT:{w:96,h:138},portrait:null,portraitPreset:3,portraitPreparedCache:null,portraitPalette:[0,1,2,255],portraitSubjectPalette:[30,90,170,255],fixture:null});
vm.runInContext(algorithm,context);
function rgba(values){return Uint8ClampedArray.from(values.flatMap(v=>[v,v,v,255]))}
const tones=rgba([0,80,150,255]);
assert.deepEqual(Array.from(context.portraitLevels(tones,4,1,false,0,2).levels),[0,1,2,3]);
const transparent=rgba([255,80,150,255]);transparent[3]=0;
assert.equal(context.portraitLevels(transparent,4,1,false,0).mask[0],0);
assert.equal(context.portraitLevels(tones,4,1,false,0).mask[0],1,'opaque black must remain independent of transparency');
const solid=rgba(new Array(64).fill(255));
for(const p of [27,28,35,36])solid.set([110,110,110,255],p*4);
const cut=context.portraitLevels(solid,8,8,true,0);
assert.equal(cut.mask[0],0);assert.equal(cut.mask[27],1);
assert.equal(context.portraitLevels(solid,8,8,false,0).mask[0],1);
const blank=context.portraitLevels(rgba(new Array(64).fill(255)),8,8,true,0);
assert.ok(Array.from(blank.mask).every(v=>v===0));
for(const preset of [1,2,3]){
 const low=context.portraitLevels(tones,4,1,false,-48,preset).levels;
 const high=context.portraitLevels(tones,4,1,false,48,preset).levels;
 assert.ok(high.every((v,i)=>v>=low[i]));
}
context.fixture=rgba(Array.from({length:96*138},(_,i)=>i%256));
context.fixture[3]=0;context.fixture.set([0,0,0,255],4);
vm.runInContext('function portraitPixels(){return fixture}',context);
const variants=[];
for(const preset of [1,2,3]){
 context.setPortraitPreset(preset);
 const packed=context.portraitPacked();variants.push(Buffer.from(packed));
 assert.equal(packed.length,4976);
 assert.deepEqual(Array.from(packed.slice(0,8)),[80,84,51,53,preset,0,0,0]);
 assert.equal(packed[8+3312]&128,0);assert.ok(packed[8+3312]&64);
 assert.equal(context.portraitColor(packed,0),0,'transparent pixel uses homepage background');
 if(preset!==1)assert.equal(context.portraitColor(packed,1),30,'opaque black uses the darkest subject color');
 const levels=Array.from({length:96*138},(_,p)=>(packed[8+(p>>2)]>>(6-2*(p&3)))&3);
 assert.ok(levels.every(v=>preset!==1||v===0||v===3));
 assert.deepEqual(Array.from(context.portraitPacked()),Array.from(packed),'preview and save are deterministic');
}
assert.notDeepEqual(variants[0],variants[1]);
assert.notDeepEqual(variants[1].subarray(8),variants[2].subarray(8),'hybrid changes actual pixels');
assert.deepEqual(variants[1].subarray(8+3312),variants[2].subarray(8+3312),'presets share one silhouette');
context.setPortraitPreset(999);assert.equal(elements.portraitPresetHybrid.checked,true);
console.log('Portrait presets: three distinct renders, masks, dark foreground, brightness, header and packing: PASS');
