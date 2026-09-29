const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs/promises');
const path=require('node:path');
const os=require('node:os');
const {spawn}=require('node:child_process');
const net=require('node:net');
const root=path.resolve(__dirname,'../..');
const pause=ms=>new Promise(r=>setTimeout(r,ms));
test('real C++ HTTP host: auth, paths, settings, core lifecycle and AI/API',{timeout:90000},async()=>{
 const dir=await fs.mkdtemp(path.join(os.tmpdir(),'ppp-web-test-'));
 const probe=net.createServer();await new Promise(r=>probe.listen(0,'127.0.0.1',r));const port=probe.address().port;await new Promise(r=>probe.close(r));
 const cfg=JSON.parse(await fs.readFile(path.join(root,'appsettings.json'),'utf8'));cfg.client.server='ppp://127.0.0.1:9/';cfg.ip.public='127.0.0.1';cfg.ip.interface='127.0.0.1';cfg.vmem={size:0,path:''};cfg.ip={interface:'0.0.0.0',public:'0.0.0.0'};cfg.client['server-proxy']='';cfg.client.mappings=[];cfg.client.log='';cfg.client['paper-airplane'].tcp=false;cfg.client['http-proxy']={bind:'127.0.0.1',port:0};cfg.client['socks-proxy']={bind:'127.0.0.1',port:0};cfg.udp.static.servers=[];cfg.udp.static.aggligator=0;cfg.udp.static.icmp=false;
 await fs.writeFile(path.join(dir,'test-server.json'),JSON.stringify(cfg));await fs.writeFile(path.join(dir,'ip.txt'),'127.0.0.0/8\n');
 const settings={working_dir:dir,mode:'proxy',tun_enabled:false,config_path:'./test-server.json',server_dir:'.',proxy_http_port:'0',proxy_socks_port:'0',system_proxy_enabled:false,auto_restart:'0',log_file:'./core.log',rpc_listen:'',rpc_token:'',bypass_mode:'no'};
 await fs.writeFile(path.join(dir,'ppp-web.json'),JSON.stringify(settings));
 // Exercise the fresh-device distribution: copy ONLY the executable, with no web/ or icon files.
 const executable=path.join(dir,'ppp-web.exe');
 await fs.copyFile(path.join(root,'web/dist/ppp-web.exe'),executable);
 const child=spawn(executable,['--base='+dir,'--port='+port,'--no-browser'],{windowsHide:true,stdio:'ignore'});const exited=new Promise(r=>child.once('exit',r));const url='http://127.0.0.1:'+port;let token;
 try{
  for(let i=0;i<80;i++){try{const r=await fetch(url+'/session.js');if(r.ok){token=JSON.parse((await r.text()).match(/=(.*);/s)[1]);break;}}catch{}await pause(100);}assert.ok(token,'Host is reachable');
  const call=async(method,params={})=>{const r=await fetch(url+'/api/rpc',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+token},body:JSON.stringify({id:1,method,params})});assert.equal(r.status,200);return r.json();};
  for(const [endpoint,source,type] of [['/','web/index.html','text/html'],['/style.css','web/style.css','text/css'],['/app.js','web/app.js','text/javascript'],['/favicon.ico','icon.ico','image/x-icon']]){
   const response=await fetch(url+endpoint);assert.equal(response.status,200);assert.ok(response.headers.get('content-type').startsWith(type));
   assert.deepEqual(Buffer.from(await response.arrayBuffer()),await fs.readFile(path.join(root,source)),'Embedded asset matches source: '+endpoint);
  }
  let r=await fetch(url+'/api/rpc',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});assert.equal(r.status,401);
  r=await fetch(url+'/api/rpc',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+token,Origin:'https://untrusted.example'},body:'{}'});assert.equal(r.status,403);
  const hostCode=await new Promise((resolve,reject)=>{require('node:http').get(url+'/session.js',{headers:{Host:'untrusted.example'}},r=>{r.resume();resolve(r.statusCode)}).on('error',reject)});assert.equal(hostCode,403);
  r=await fetch(url+'/../host.cpp');assert.equal(r.status,404);
  const paths=await call('host.paths',{paths:['./ip.txt','./missing.txt']});assert.equal(paths.result[0].exists,true);assert.equal(paths.result[1].exists,false);
  assert.equal((await call('host.status')).result.running,false);
  const catalog=await call('host.catalog');assert.ok(catalog.result.some(r=>r.name==='test-server'));
  assert.equal((await call('host.save',{settings:{dns:'1.1.1.1,8.8.8.8'}})).ok,true);
  assert.equal(JSON.parse(await fs.readFile(path.join(dir,'ppp-web.json'),'utf8')).dns,'1.1.1.1,8.8.8.8');
  const start=await call('host.start');assert.equal(start.ok,true,JSON.stringify(start));assert.equal(start.result.running,true);
  const manifest=await call('describe_api');assert.equal(manifest.ok,true);assert.ok(manifest.result.methods.some(m=>m.name==='get_snapshot'||m.method==='get_snapshot'));
  const snapshot=await call('get_snapshot');assert.equal(snapshot.ok,true);assert.equal(snapshot.result.network.mode,'proxy-only');
  assert.equal((await call('switch_server',{tag:'does-not-exist'})).result.accepted,false);
  const restart=await call('host.restart');assert.equal(restart.ok,true,JSON.stringify(restart));
  assert.equal((await call('host.stop')).result.running,false);
  assert.equal((await call('get_snapshot')).ok,false);
  assert.equal((await call('host.status')).ok,true,'Web host survives core shutdown');
  assert.equal((await call('host.exit')).ok,true);await Promise.race([exited,pause(8000)]);assert.notEqual(child.exitCode,null,'Host exits cleanly');
 }catch(e){await pause(500);console.error('test directory',dir,'exit',child.exitCode);try{console.error((await fs.readFile(path.join(dir,'core.log'),'utf8')).slice(-4000))}catch{}throw e;}finally{if(child.exitCode===null){child.kill();await exited;}assert.equal(path.dirname(path.resolve(dir)),path.resolve(os.tmpdir()));assert.ok(path.basename(dir).startsWith('ppp-web-test-'));await fs.rm(dir,{recursive:true,force:true});}
});
