const {chromium}=require('C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true,args:['--enable-unsafe-swiftshader']});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1000}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.argv[2]);
  await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('三角形'));
  const camera=()=>{const gl=document.querySelector('canvas').getContext('webgl'),p=gl.getParameter(gl.CURRENT_PROGRAM);return ['eye','rot','pan','zoom'].map(n=>{const v=gl.getUniform(p,gl.getUniformLocation(p,n));return typeof v==='number'?v:Array.from(v);});};
  const before=await page.evaluate(camera);
  await page.screenshot({path:process.argv[3]+'/expanded.png'});
  await page.locator('#panelToggle').click();
  assert.equal(await page.locator('#panelToggle').getAttribute('aria-expanded'),'false');
  assert.equal(await page.locator('#panelContent').isVisible(),false);
  assert((await page.locator('#panel').boundingBox()).width<150);
  assert.deepEqual(await page.evaluate(camera),before);
  await page.screenshot({path:process.argv[3]+'/collapsed.png'});
  await page.keyboard.press('Enter');
  assert.equal(await page.locator('#panelContent').isVisible(),true);
  assert.equal(await page.locator('#panelToggle').getAttribute('aria-expanded'),'true');
  await page.locator('#filter').selectOption('all');
  await page.waitForTimeout(100);
  const allStatus=await page.locator('#status').textContent();
  await page.locator('#filter').selectOption('trigger');
  await page.locator('#top').click();
  await page.waitForTimeout(100);
  const triggerStatus=await page.locator('#status').textContent();
  await page.screenshot({path:process.argv[3]+'/triggers-top.png'});
  if(await page.locator('#attributeLegend').isVisible()){
   await page.locator('#filter').selectOption('wreck');
   await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('16 个对象'));
   await page.locator('#focus').click();
   await page.screenshot({path:process.argv[3]+'/wreck.png'});
   await page.locator('#filter').selectOption('respawn');
   await page.waitForFunction(()=>document.querySelector('#status').textContent.startsWith('1 个对象'));
   await page.locator('#focus').click();
   await page.waitForTimeout(100);
   const focused=await page.evaluate(camera);
   assert(focused.flat().every(Number.isFinite));
   assert.deepEqual(await page.evaluate(()=>{const g=document.querySelector('canvas').getContext('webgl');return Array.from(g.getUniform(g.getParameter(g.CURRENT_PROGRAM),g.getUniformLocation(g.getParameter(g.CURRENT_PROGRAM),'color'))).map(x=>Math.round(x*100));}),[45,93,67]);
   await page.screenshot({path:process.argv[3]+'/respawn.png'});
   assert.match(await page.locator('#object option[value="101"]').textContent(),/respawn/);
   await page.locator('#object').selectOption('22');
   await page.locator('#focus').click();
   assert.match(await page.locator('#status').textContent(),/没有对象/);
   assert.deepEqual(await page.evaluate(camera),focused);
   console.log('Attributes: wreck=16, respawn=1, native label and GPU color verified; focus and empty scope verified');
  }
  assert.deepEqual(errors,[]);
  console.log('Browser: rendered, collapse, expand with keyboard, compact bounds, camera unchanged, no page errors');
  console.log({allStatus,triggerStatus});
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
