#pragma once

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Реле плавного пуску</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,'Segoe UI',Roboto,sans-serif;background:#0f1419;color:#e6edf3;min-height:100vh;padding:16px}
.wrap{max-width:680px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;gap:10px}
h1{font-size:1.2rem}
h2{font-size:1rem;margin-bottom:14px;color:#9ab0c6}
.badge{padding:6px 14px;border-radius:999px;font-weight:600;font-size:.85rem;white-space:nowrap}
.badge.idle{background:#2d333b;color:#adbac7}
.badge.ramp{background:#3d2b1f;color:#f0a34a;animation:pulse 1s infinite}
.badge.run{background:#1f3d2b;color:#57d38c}
.badge.down{background:#1f2d3d;color:#6cb0ff}
.badge.fault{background:#3d1f1f;color:#ff7b72;animation:pulse .7s infinite}
@keyframes pulse{50%{opacity:.5}}
.card{background:#161b22;border:1px solid #2d333b;border-radius:12px;padding:18px;margin-bottom:16px}
label{display:block;margin:12px 0 4px;font-size:.9rem;color:#adbac7}
label b{color:#e6edf3}
input[type=range]{width:100%;accent-color:#4da3ff;cursor:pointer}
input[type=text],input[type=password]{width:100%;padding:10px;margin:6px 0 12px;border-radius:8px;border:1px solid #2d333b;background:#0f1419;color:#e6edf3;font-size:.95rem}
.chk{display:flex;gap:10px;align-items:center;margin:16px 0}
.chk input{width:18px;height:18px;accent-color:#4da3ff}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:16px 0 6px}
.btn{padding:14px;border:none;border-radius:10px;font-size:1rem;font-weight:700;color:#fff;cursor:pointer;width:100%}
.btn:active{transform:scale(.97)}
.btn.start{background:#2ea043}
.btn.stop{background:#da3633}
.btn.save{background:#1f6feb;width:auto;padding:10px 24px;font-size:.9rem}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}
.stats div{background:#0f1419;border-radius:8px;padding:10px 12px;display:flex;justify-content:space-between;align-items:center}
.stats span{color:#768390;font-size:.78rem}
.stats b{font-variant-numeric:tabular-nums}
.levelbar{height:10px;background:#0f1419;border-radius:6px;overflow:hidden;margin-top:6px}
#lvfill{height:100%;width:0;background:linear-gradient(90deg,#4da3ff,#2ea043);transition:width .3s}
.fault{margin-top:14px;padding:10px 12px;background:#3d1f1f;border-radius:8px;color:#ff7b72;font-size:.9rem}
.hidden{display:none}
.hint{margin-top:10px;color:#768390;font-size:.85rem;line-height:1.5}
.okmsg{color:#57d38c;font-size:.9rem;margin-left:12px}
footer{text-align:center;color:#768390;font-size:.8rem;margin:18px 0}
</style>
</head>
<body>
<div class="wrap">
<header><h1>&#9889; Реле плавного пуску 2 кВт</h1><div id="badge" class="badge idle">&mdash;</div></header>

<div class="card">
<h2>Керування</h2>
<div class="levelbar"><div id="lvfill"></div></div>
<div class="row2">
<button class="btn start" onclick="cmd('/api/start')">&#9654; ПУСК</button>
<button class="btn stop" onclick="cmd('/api/stop')">&#9632; СТОП</button>
</div>
<div class="stats">
<div><span>Рівень напруги</span><b id="level">&mdash;</b></div>
<div><span>Струм</span><b id="cur">&mdash;</b></div>
<div><span>Потужність</span><b id="pow">&mdash;</b></div>
<div><span>Частота мережі</span><b id="freq">&mdash;</b></div>
<div><span>Мережа 220В</span><b id="mains">&mdash;</b></div>
</div>
<div id="fault" class="fault hidden"></div>
</div>

<div class="card">
<h2>Налаштування пуску</h2>
<label>Стартова напруга: <b id="vStartV"></b></label>
<input type="range" id="vStart" min="10" max="80" step="1">
<label>Час розгону: <b id="vRampV"></b></label>
<input type="range" id="vRamp" min="1" max="60" step="1">
<label>Захист за струмом: <b id="vMaxV"></b></label>
<input type="range" id="vMax" min="0" max="40" step="1">
<label class="chk"><input type="checkbox" id="vSoft"> М&rsquo;яка зупинка (плавне зниження напруги)</label>
<button class="btn save" onclick="saveSettings()">Зберегти</button><span id="savedMsg" class="okmsg"></span>
</div>

<div class="card">
<h2>Wi-Fi</h2>
<input type="text" id="wSsid" maxlength="32" placeholder="SSID вашої мережі">
<input type="password" id="wPass" maxlength="63" placeholder="Пароль (8+ символiв)">
<button class="btn save" onclick="saveWifi()">Зберегти та перезавантажити</button>
<span id="wifiMsg" class="okmsg"></span>
<div class="hint" id="wifiInfo"></div>
</div>

<footer>ESP32-C3 Mini &bull; v1.0</footer>
</div>
<script>
const $=i=>document.getElementById(i);
const states={0:['Готовий','idle'],1:['Розгін','ramp'],2:['Робота','run'],3:['Гальмування','down'],4:['Аварія','fault'],5:['Вимкнення','down']};
function poll(){
fetch('/api/status').then(r=>r.json()).then(s=>{
const st=states[s.state]||['?','idle'];
const b=$('badge');b.textContent=st[0];b.className='badge '+st[1];
const lv=s.state===2?100:s.level;
$('lvfill').style.width=lv+'%';
$('level').textContent=lv+' %';
$('cur').textContent=s.current<0?'—':s.current.toFixed(1)+' А';
$('pow').textContent=s.current>0.3?Math.round(s.current*230)+' Вт':'—';
$('freq').textContent=s.freq.toFixed(1)+' Гц';
$('mains').textContent=s.mains?'\u2705 \u0454':'\u26A0 \u043d\u0435\u043c\u0430\u0454';
const f=$('fault');
if(s.state===4&&s.fault){f.textContent='\u26A0 '+s.fault;f.classList.remove('hidden')}else f.classList.add('hidden');
$('wifiInfo').textContent=s.ap?'Режим точки доступу: '+s.ssid+' (пароль 12345678), IP 192.168.4.1':'Підключено до: '+s.ssid+' \u2022 RSSI '+s.rssi+' dBm';
}).catch(()=>{});
}
setInterval(poll,1000);poll();
function loadSettings(){
fetch('/api/settings').then(r=>r.json()).then(c=>{
$('vStart').value=c.startPct;$('vRamp').value=c.rampTime;$('vMax').value=c.maxCurrent;$('vSoft').checked=c.softStop;upd();
}).catch(()=>{});
}
function upd(){
$('vStartV').textContent=$('vStart').value+' %';
$('vRampV').textContent=$('vRamp').value+' с';
$('vMaxV').textContent=$('vMax').value==='0'?'вимкнено':$('vMax').value+' А';
}
['vStart','vRamp','vMax'].forEach(i=>$(i).addEventListener('input',upd));
loadSettings();
function saveSettings(){
const p=new URLSearchParams({startPct:$('vStart').value,rampTime:$('vRamp').value,maxCurrent:$('vMax').value,softStop:$('vSoft').checked?'1':'0'});
fetch('/api/settings',{method:'POST',body:p}).then(()=>{
$('savedMsg').textContent='\u2713 Збережено';setTimeout(()=>$('savedMsg').textContent='',2000);
}).catch(()=>{});
}
function cmd(u){fetch(u,{method:'POST'}).catch(()=>{});}
function saveWifi(){
const ssid=$('wSsid').value.trim();
if(!ssid){alert('Введіть SSID');return;}
const p=new URLSearchParams({ssid:ssid,pass:$('wPass').value});
fetch('/api/wifi',{method:'POST',body:p}).then(()=>{
$('wifiMsg').textContent='\u2713 Збережено, перезавантаження...';
}).catch(()=>{});
}
</script>
</body>
</html>)rawliteral";
