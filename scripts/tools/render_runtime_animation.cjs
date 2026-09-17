/** Export the real canvas timeline to README GIFs. Requires playwright + Pillow.
 * NODE_PATH may point to the installed playwright package directory.
 * BROWSER_EXECUTABLE_PATH and PYTHON select a local browser and Python runtime.
 * Usage: node scripts/tools/render_runtime_animation.cjs [zh|en|both]
 */
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {pathToFileURL} = require('node:url');
const {spawnSync} = require('node:child_process');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../..');
(async()=>{
 const language = process.argv[2] || 'both';
 if (!['zh','en','both'].includes(language)) throw Error('Expected zh, en or both');
 const languages = language === 'both' ? ['zh','en'] : [language];
 const browser = await chromium.launch({headless:true,
   ...(process.env.BROWSER_EXECUTABLE_PATH ? {executablePath:process.env.BROWSER_EXECUTABLE_PATH} : {})});
 try {
  const page = await browser.newPage({viewport:{width:1320,height:960}});
  page.on('pageerror', error=>{throw error});
  await page.goto(pathToFileURL(path.join(root,'docs/assets/readme/runtime-animation.html')).href);
  await page.evaluate(()=>window.freezeAnimation());
  for (const lang of languages) {
   const frames = fs.mkdtempSync(path.join(os.tmpdir(),`labbridge-runtime-${lang}-`));
   await page.evaluate(lang=>window.setLanguage(lang),lang);
   for (let i=0;i<450;i++) {
    const data = await page.evaluate(t=>{window.renderFrame(t);return document.querySelector('canvas').toDataURL('image/png').split(',')[1]},i*.08);
    fs.writeFileSync(path.join(frames,`${String(i).padStart(4,'0')}.png`),Buffer.from(data,'base64'));
    if(i%100===0) console.log(`${lang}: ${i}/450 frames`);
   }
   const poster = await page.evaluate(()=>{window.renderFrame(7.2);return document.querySelector('canvas').toDataURL('image/png').split(',')[1]});
   fs.writeFileSync(path.join(frames,'poster.png'),Buffer.from(poster,'base64'));
   const result=spawnSync(process.env.PYTHON||'python3',[path.join(__dirname,'render_readme_media.py'),
     '--runtime-frames',frames,'--runtime-only','--language',lang],{stdio:'inherit'});
   if(result.status!==0) throw Error('GIF encoding failed');
   console.log(`Source frames retained: ${frames}`);
  }
 } finally {await browser.close()}
})().catch(error=>{console.error(error);process.exitCode=1});
