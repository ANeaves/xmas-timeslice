#ifndef WEBUI_H
#define WEBUI_H

// Simple test console served at "/" in both station and fallback-AP mode.
static const char WEBUI_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Cam Test</title>
<style>
body{font-family:system-ui,sans-serif;max-width:720px;margin:1em auto;padding:0 1em}
button{padding:.6em 1em;margin:.2em;font-size:1em}
td{padding:2px 14px 2px 0}
#img{max-width:100%;border:1px solid #888;display:none;margin-top:.5em}
.bad{color:#b00}.ok{color:#080}
fieldset{margin-top:1.5em}
</style></head><body>
<h2 id="title">ESP32 Cam</h2>
<div>
<button onclick="arm()">Arm</button>
<button onclick="trigger()">Trigger</button>
<button onclick="disarm()">Disarm</button>
<button onclick="lock(1)">Lock exposure</button>
<button onclick="lock(0)">Unlock</button>
<a href="/ui" target="_blank">Full camera UI (aim / focus)</a>
<a href="/setup">Settings</a>
</div>
<p id="msg"></p>
<table id="st"></table>
<img id="img" alt="last shot">
<p>Hardware trigger: pulse GPIO13 high (3.3 V) while armed. A powered-down camera ignores the pin.</p>
<fieldset><legend>Firmware update</legend>
<input type="file" id="fw" accept=".bin"><br>
<input type="password" id="pw" placeholder="OTA password">
<button onclick="flash()">Upload &amp; reboot</button>
</fieldset>
<script>
const $=id=>document.getElementById(id);
let last=null;
function say(t,bad){const m=$('msg');m.textContent=t;m.className=bad?'bad':'ok';}
async function call(p){
  try{
    const r=await fetch(p,{method:'POST'});const j=await r.json();
    if(!r.ok||j.ok===false)say(p+': '+(j.error||r.status),true);
    return j;
  }catch(e){say(p+': '+e,true);return null;}
}
function render(j){
  last=j;
  $('title').textContent='ESP32 Cam #'+j.cam_id+' ('+j.mode.toUpperCase()+' '+j.ip+')';
  const rows=[
    ['state',j.state],['exposure locked',j.locked],['shot id',j.shot_id],
    ['size',j.size?(j.size/1024).toFixed(0)+' KB':'-'],
    ['resolution',j.width?j.width+'x'+j.height:'-'],
    ['trigger to stored',j.shot_id?j.latency_ms+' ms':'-'],
    ['missed triggers',j.missed],
    ['auto power-down in',!j.auto_disarm?'never (disabled)':j.state=='armed'?(j.idle_ms/1000).toFixed(0)+' s':'-'],
    ['wifi rssi',j.rssi],['uptime',(j.uptime_ms/1000).toFixed(0)+' s'],
    ['last error',j.error||'-']];
  $('st').innerHTML=rows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>').join('');
}
async function refresh(){
  const j=await fetch('/shot_status').then(r=>r.json()).catch(()=>null);
  if(j)render(j);
}
async function arm(){
  say('Arming (a few seconds)...');
  const j=await call('/arm');
  if(!j||!j.ok)return;
  for(let i=0;i<60;i++){  // arming runs in the background on the camera; poll until it finishes
    await refresh();
    if(!last||last.state!='arming')break;
    await new Promise(r=>setTimeout(r,250));
  }
  if(last&&last.state=='armed')say('Armed');else say('Arm failed: '+((last&&last.error)||'timed out'),true);
}
async function disarm(){const j=await call('/disarm');if(j&&j.ok)say('Powered down');refresh();}
async function lock(on){const j=await call(on?'/lock':'/unlock');if(j&&j.ok)say(on?'Exposure locked':'Auto exposure');refresh();}
async function trigger(){
  const before=last?last.shot_id:0;
  const j=await call('/trigger');
  if(!j||!j.ok)return;
  say('Triggered, waiting for image...');
  for(let i=0;i<80;i++){
    await new Promise(r=>setTimeout(r,250));
    await refresh();
    if(last&&last.shot_id>before){
      const im=$('img');im.src='/shot?id='+last.shot_id;im.style.display='block';
      say('Shot #'+last.shot_id+' captured');return;
    }
  }
  say('Timed out waiting for the shot',true);
}
function flash(){
  const f=$('fw').files[0];
  if(!f){say('Choose a .bin file first',true);return;}
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.setRequestHeader('X-Auth',$('pw').value);
  x.upload.onprogress=e=>{if(e.lengthComputable)say('Uploading '+Math.round(100*e.loaded/e.total)+'%');};
  x.onload=()=>say(x.status==200?'Update OK - rebooting, reconnect in ~10 s':'Update failed: '+x.responseText,x.status!=200);
  x.onerror=()=>say('Upload error',true);
  x.send(f);
}
setInterval(refresh,1000);refresh();
</script></body></html>
)rawliteral";

#endif  // WEBUI_H
