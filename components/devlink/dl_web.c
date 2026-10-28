/*
 * Config-AP setup page (SPEC §5.4): scan list, SSID/password, friendly name, Save. Uses the same JSON API.
 * The API key is fixed at build time and not a secret on a bench network (SPEC §1), so the page carries it.
 */
#include <stdio.h>

#include "sdkconfig.h"

static const char k_page[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>devlink setup</title><style>"
    "body{font:15px system-ui,sans-serif;margin:0;padding:16px;max-width:420px;color:#222;background:#fafafa}"
    "h1{font-size:19px;margin:0 0 4px}small{color:#666}label{display:block;margin:12px 0 4px;font-weight:600}"
    "input,select,button{width:100%;box-sizing:border-box;padding:9px;font:inherit;border:1px solid #bbb;"
    "border-radius:6px;background:#fff}button{margin-top:16px;background:#1f6feb;color:#fff;border:0}"
    "#msg{margin-top:12px;min-height:1.2em}</style></head><body>"
    "<h1>devlink setup</h1><small id=who>loading...</small>"
    "<label for=net>Network</label><select id=net><option>scanning...</option></select>"
    "<label for=ssid>SSID</label><input id=ssid autocomplete=off>"
    "<label for=pw>Password</label><input id=pw type=password autocomplete=off>"
    "<label for=lab>Name</label><input id=lab maxlength=32>"
    "<button id=save>Save</button><div id=msg></div><script>"
    "const K='" CONFIG_DEVLINK_API_KEY "';let mac='';const $=i=>document.getElementById(i);"
    "const H=()=>({'Content-Type':'application/json','X-Api-Key':K,'X-Device-Id':mac});"
    "fetch('/api/v1/info').then(r=>r.json()).then(i=>{mac=i.mac;$('lab').value=i.label;"
    "$('who').textContent=(i.label||i.hostname)+' \\u00b7 '+i.mode+' \\u00b7 '+i.fw.version+' \\u00b7 '+i.mac});"
    "fetch('/api/v1/wifi/scan').then(r=>r.json()).then(s=>{const n=$('net');n.innerHTML='<option value=\"\">'"
    "+'(choose)</option>';s.networks.forEach(w=>{const o=document.createElement('option');o.value=w.ssid;"
    "o.textContent=w.ssid+'  '+w.rssi+' dBm'+(w.secure?'':' (open)');n.appendChild(o)})}).catch(()=>{});"
    "$('net').onchange=e=>{if(e.target.value)$('ssid').value=e.target.value};"
    "$('save').onclick=async()=>{$('msg').textContent='saving...';try{"
    "await fetch('/api/v1/config',{method:'POST',headers:H(),body:JSON.stringify({label:$('lab').value})});"
    "if($('ssid').value){const r=await fetch('/api/v1/wifi',{method:'POST',headers:H(),"
    "body:JSON.stringify({ssid:$('ssid').value,password:$('pw').value})});const j=await r.json();"
    "$('msg').textContent=r.ok?'Saved. The board is connecting to '+$('ssid').value+'.':'Error: '+j.error}"
    "else $('msg').textContent='Name saved.'}catch(e){$('msg').textContent='Error: '+e}};"
    "</script></body></html>";

const char *dl_web_page(void)
{
    return k_page;
}
