const {chromium}=require('C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true,args:['--enable-unsafe-swiftshader']});
try{const page=await browser.newPage({viewport:{width:1440,height:1000}}),errors=[];
page.on('pageerror',e=>errors.push(e.message));await page.goto(process.argv[2]);
await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('三角形'));
const results={};
for(const [tag,triangles] of [['wreck',366],['respawn',1877],['ramp',1099],['nochassis',8839],['magnet',0],['highjump',0]]){
await page.locator('#filter').selectOption(tag);
await page.waitForTimeout(80);
const text=await page.locator('#status').textContent();assert(text.includes(triangles.toLocaleString('en-US')+' 个三角形'));results[tag]=text;
if(triangles){await page.locator('#focus').click();await page.waitForTimeout(80);await page.screenshot({path:process.argv[3]+'/'+tag+'.png'});}
}
assert.deepEqual(errors,[]);console.log(results);
fs.writeFileSync(process.argv[3]+'/browser-acceptance.json',JSON.stringify({results,errors},null,2));
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
