import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=fs.readFileSync(new URL('./track_viewer_template.html',import.meta.url),'utf8');
new vm.Script(html.match(/<script>([\s\S]*)<\/script>/)[1]);
const advance=html.slice(html.indexOf('function advance(now)'),html.indexOf('const attr='));
function movement(code,frames){
 const ctx=vm.createContext({Math,performance:{now:()=>0}});
 vm.runInContext(`let extent=1000,last=100,mode={value:'fly'},keys=new Set(['${code}']),yaw=0,pitch=0,eye=[0,0,0],speed={value:'0'};${advance}`,ctx);
 for(let i=1;i<=frames;i++)vm.runInContext(`advance(${100+i*1000/frames})`,ctx);
 return Array.from(vm.runInContext('eye',ctx));
}
assert(movement('KeyW',60)[2]<0);
assert(movement('KeyS',60)[2]>0);
assert(movement('KeyA',60)[0]<0);
assert(movement('KeyD',60)[0]>0);
assert(movement('Space',60)[1]>0);
assert(movement('ShiftLeft',60)[1]<0);
assert(Math.abs(movement('KeyW',60)[2]-movement('KeyW',120)[2])<1e-10);
assert(html.includes('canvas.onblur=stop'));
const panelNodes={
 '#panel':{classList:{toggle(name,value){this[name]=value;}}},
 '#panelToggle':{setAttribute(name,value){this[name]=value;}},
 '#panelContent':{hidden:false}
};
const panelContext=vm.createContext({document:{querySelector:s=>panelNodes[s]}});
vm.runInContext(html.slice(html.indexOf('const panel='),html.indexOf('const scene=')),panelContext);
panelNodes['#panelToggle'].onclick();
assert.equal(panelNodes['#panelContent'].hidden,true);
assert.equal(panelNodes['#panelToggle']['aria-expanded'],'false');
assert.equal(panelNodes['#panelToggle'].textContent,'展开工具');
panelNodes['#panelToggle'].onclick();
assert.equal(panelNodes['#panelContent'].hidden,false);
assert.equal(panelNodes['#panelToggle']['aria-expanded'],'true');
assert.equal(panelNodes['#panelToggle'].textContent,'收起');
console.log('Flight controls: syntax, 6 directions, frame-rate independence, blur reset passed');
