const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs/promises');
const path=require('node:path');
const os=require('node:os');
const net=require('node:net');
const {spawn,execFile}=require('node:child_process');
const execFileAsync=require('node:util').promisify(execFile);
const pause=ms=>new Promise(r=>setTimeout(r,ms));
test('default 19999 conflict: bind another port, expose tray icon, and exit from tray window',{timeout:30000},async()=>{
 const directory=await fs.mkdtemp(path.join(os.tmpdir(),'ppp-web-port-test-'));
 const blocker=net.createServer(s=>s.destroy());
 let ownedBlocker=false;
 await new Promise((resolve,reject)=>{blocker.once('error',e=>e.code==='EADDRINUSE'?resolve():reject(e));blocker.listen(19999,'127.0.0.1',()=>{ownedBlocker=true;resolve()})});
 const executable=process.env.PPP_WEB_TEST_EXE||path.resolve(__dirname,'../dist/ppp-web.exe');
 const child=spawn(executable,['--base='+directory,'--no-browser'],{windowsHide:true,stdio:'ignore'});
 const ended=new Promise(r=>child.once('exit',r));
 try{
  let url;
  for(let i=0;i<100;i++){try{url=(await fs.readFile(path.join(directory,'ppp-web-address.txt'),'utf8')).trim();break}catch{}await pause(100)}
  assert.ok(url,'Actual endpoint was published');assert.equal(new URL(url).hostname,'127.0.0.1');assert.notEqual(new URL(url).port,'19999');
  const session=await (await fetch(url+'session.js')).text();const token=JSON.parse(session.match(/=(.*);/s)[1]);
  const call=async (method,params={})=>(await fetch(url+'api/rpc',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+token},body:JSON.stringify({method,params})})).json();
  const status=await call('host.status');assert.equal(status.ok,true);assert.equal(status.result.web_url+'/',url);assert.equal(status.result.running,false);
  assert.equal((await call('host.save',{settings:{web_port:'70000'}})).ok,false);
  assert.equal((await call('host.save',{settings:{web_port:'21000'}})).ok,true);
  assert.equal(JSON.parse(await fs.readFile(path.join(directory,'ppp-web.json'),'utf8')).web_port,'21000');
  await execFileAsync('powershell.exe',['-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',path.join(__dirname,'close-tray.ps1'),'-HostProcessId',String(child.pid)],{windowsHide:true,timeout:15000});
  for(let i=0;i<30&&child.exitCode===null;i++)await pause(100);
  assert.equal(child.exitCode,0,'Tray close exits the host cleanly');
  await assert.rejects(fetch(url+'session.js'), 'Tray exit closes the HTTP listener');
  if(ownedBlocker)assert.equal(blocker.listening,true,'Occupying application was not disturbed');
 }finally{if(child.exitCode===null){child.kill();await ended}if(ownedBlocker)await new Promise(r=>blocker.close(r));assert.equal(path.dirname(path.resolve(directory)),path.resolve(os.tmpdir()));assert.ok(path.basename(directory).startsWith('ppp-web-port-test-'));await fs.rm(directory,{recursive:true,force:true})}
});
