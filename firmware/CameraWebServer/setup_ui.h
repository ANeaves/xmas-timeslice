#ifndef SETUP_UI_H
#define SETUP_UI_H

// Settings page served at /setup. Loads GET /config, saves with PATCH /config, resets with DELETE /config.
static const char SETUP_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Cam Setup</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:1em auto;padding:0 1em}
label{display:block;margin-top:.6em;font-weight:600}
input,select{width:100%;padding:.4em;font-size:1em;box-sizing:border-box}
input[type=checkbox]{width:auto}
button{padding:.6em 1em;margin:.8em .4em 0 0;font-size:1em}
fieldset{margin-top:1em}
small{font-weight:400;color:#555}
.bad{color:#b00}.ok{color:#080}
</style></head><body>
<h2 id="title">Camera setup</h2>
<p id="banner"></p>
<p id="msg"></p>

<fieldset><legend>Authentication</legend>
<label>Admin password <small>(required to save; factory default is in config.h)</small>
<input type="password" id="admin" autocomplete="off"></label>
</fieldset>

<fieldset><legend>Identity</legend>
<label>Camera ID (0-7)<input type="number" id="cam_id" min="0" max="7"></label>
</fieldset>

<fieldset><legend>Raspberry Pi network</legend>
<label>WiFi SSID<input id="ssid" maxlength="32"></label>
<label>WiFi password <small>(leave blank to keep the saved one)</small><input type="password" id="wifi_pass" maxlength="64" autocomplete="off"></label>
<label>IP prefix <small>(first three numbers)</small><input id="ip_prefix" placeholder="192.168.4"></label>
<label>Host base <small>(camera IP = prefix.(base + ID))</small><input type="number" id="host_base" min="1" max="247"></label>
<label>Gateway (the Pi)<input id="gateway"></label>
<label>Netmask<input id="netmask"></label>
<p id="ipnote"></p>
</fieldset>

<fieldset><legend>Setup / fallback access point</legend>
<label>AP password <small>(blank = keep, min 8 characters)</small><input type="password" id="ap_pass" maxlength="63" autocomplete="off"></label>
<label>AP channel<input type="number" id="ap_channel" min="1" max="13"></label>
<label>New admin password <small>(blank = keep)</small><input type="password" id="new_admin_pass" maxlength="63" autocomplete="off"></label>
</fieldset>

<fieldset><legend>Camera</legend>
<label>Resolution<select id="framesize"></select></label>
<label>JPEG quality <small>(0-63, lower = better / bigger file)</small><input type="number" id="quality" min="0" max="63"></label>
<label><input type="checkbox" id="vflip"> Flip vertically</label>
<label><input type="checkbox" id="hmirror"> Mirror horizontally</label>
<label>Warm-up after arming (ms)<input type="number" id="warmup_ms" min="0" max="10000"></label>
<label>Trigger debounce (ms)<input type="number" id="debounce_ms" min="5" max="1000"></label>
</fieldset>

<fieldset><legend>Power / heat</legend>
<label>Auto power-down after (seconds idle) <small>(0 = never, otherwise 5-3600)</small><input type="number" id="idle_s" min="0" max="3600"></label>
<label>WiFi TX power<select id="tx_power"></select></label>
<label>CPU speed (MHz)<select id="cpu_mhz"><option>80</option><option>160</option><option>240</option></select></label>
</fieldset>

<button onclick="save()">Save &amp; reboot</button>
<button onclick="factory()">Reset to factory defaults</button>
<a href="/">Back to test console</a>

<script>
const $=id=>document.getElementById(id);
const TEXT=['cam_id','ssid','ip_prefix','host_base','gateway','netmask','ap_channel','framesize','quality','warmup_ms','debounce_ms','idle_s','tx_power','cpu_mhz'];
function say(t,bad){const m=$('msg');m.textContent=t;m.className=bad?'bad':'ok';}
function ipnote(){
  const id=parseInt($('cam_id').value)||0,b=parseInt($('host_base').value)||0;
  $('ipnote').textContent='This camera will use '+$('ip_prefix').value+'.'+(b+id);
}
async function load(){
  const c=await fetch('/config').then(r=>r.json());
  $('framesize').innerHTML=c.framesizes.map(o=>'<option value="'+o.v+'">'+o.n+'</option>').join('');
  $('tx_power').innerHTML=c.tx_powers.map(o=>'<option value="'+o.v+'">'+o.n+'</option>').join('');
  TEXT.forEach(k=>{$(k).value=c[k];});
  $('vflip').checked=c.vflip;$('hmirror').checked=c.hmirror;
  $('title').textContent='Camera setup (ID '+c.cam_id+')';
  $('banner').textContent=c.configured?'':'This board has not been configured yet. Set an ID and the Pi network, then save.';
  ['cam_id','host_base','ip_prefix'].forEach(k=>$(k).addEventListener('input',ipnote));
  ipnote();
}
async function save(){
  const p=new URLSearchParams();
  TEXT.forEach(k=>p.set(k,$(k).value));
  p.set('vflip',$('vflip').checked?1:0);
  p.set('hmirror',$('hmirror').checked?1:0);
  ['wifi_pass','ap_pass','new_admin_pass'].forEach(k=>{if($(k).value)p.set(k,$(k).value);});
  try{
    const r=await fetch('/config',{method:'PATCH',headers:{'X-Auth':$('admin').value,'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()});
    const j=await r.json();
    if(!r.ok||!j.ok){say(j.error||('HTTP '+r.status),true);return;}
    say('Saved. Rebooting - join the Pi network; this camera will be at '+$('ip_prefix').value+'.'+((parseInt($('host_base').value)||0)+(parseInt($('cam_id').value)||0)));
  }catch(e){say('Request failed: '+e,true);}
}
async function factory(){
  if(!confirm('Erase all saved settings? The board will reboot into setup mode.'))return;
  try{
    const r=await fetch('/config',{method:'DELETE',headers:{'X-Auth':$('admin').value}});
    const j=await r.json();
    if(!r.ok||!j.ok){say(j.error||('HTTP '+r.status),true);return;}
    say('Settings erased. Rebooting into setup mode (AP espcam-setup-xxxx).');
  }catch(e){say('Request failed: '+e,true);}
}
load();
</script></body></html>
)rawliteral";

#endif  // SETUP_UI_H
