// SPDX-License-Identifier: GPL-3.0-only
const $ = (id) => document.getElementById(id);
const root = '/data/adb/modules/ios_auto_brightness';
const quote = (s) => `'${String(s).replace(/'/g, "'\\''")}'`;
let sequence = 0, busy = false, polling = false, dirty = false;
let legacyGamma = '2.2', runtime = null, rawInvalid = false;
const defaults = new Map(Object.entries({"enabled": "1", "learning_enabled": "1", "night_enabled": "1", "low_battery_enabled": "1", "manual_gamma": "2.2", "alpha_fast": "0.45", "alpha_mid": "0.20", "alpha_slow": "0.06", "fast_threshold": "1.0", "slow_threshold": "0.05", "spike_up": "15", "spike_down": "0.05", "hyst_low": "0.025", "hyst_high": "0.03", "high_lux_threshold": "15000", "high_lux_boost_max": "0.35", "high_lux_min_pct": "0.90", "high_lux_duration_ms": "8000", "user_hold_ms": "5000", "log_max_bytes": "1048576", "log_max_backups": "2"}));
const fields = [
  ['night_enabled','夜间默认亮度降低 5%',0,1,1], ['low_battery_enabled','低于 15% 电量额外限亮',0,1,1],
  ['enabled','启用调光',0,1,1], ['learning_enabled','记住手动调节',0,1,1],
  ['alpha_fast','快速响应',.01,.9,.01],['alpha_mid','中速响应',.01,.9,.01],['alpha_slow','慢速稳定',.001,.9,.001],
  ['fast_threshold','快速变化阈值',.1,5,.05],['slow_threshold','稳定阈值',.001,.5,.001],
  ['spike_up','上升尖峰阈值',2,50,.5],['spike_down','下降尖峰阈值',.001,.5,.001],
  ['hyst_low','低亮度回差',0,.2,.005],['hyst_high','高亮度回差',0,.2,.005],
  ['high_lux_threshold','阳光模式照度阈值',5000,100000,500],['high_lux_boost_max','阳光最高增益',0,1,.01],
  ['high_lux_min_pct','强光亮度下限比例',0,1,.01],['high_lux_duration_ms','强光退出延迟（毫秒）',1000,20000,500],
  ['user_hold_ms','手动调整后等待（毫秒）',2000,30000,500],
  ['log_max_bytes','日志上限（字节）',65536,8388608,65536],['log_max_backups','日志备份份数',1,8,1],
];
function note(text) { $('notice').textContent = text; }
function exec(command) {
  return new Promise((resolve,reject) => {
    if (!window.ksu?.exec) return reject(new Error('请使用支持 ksu.exec 的 Root 管理器 WebUI（Magisk 可使用兼容的 WebUI 宿主）。'));
    const key = `iab_${Date.now()}_${sequence++}`;
    const timer = setTimeout(() => { delete window[key]; reject(new Error('服务响应超时')); },10000);
    window[key] = (code,stdout,stderr) => {clearTimeout(timer);delete window[key];Number(code) === 0 ? resolve(stdout) : reject(new Error(stderr || stdout || `错误 ${code}`));};
    try {window.ksu.exec(command,'{}',key);} catch(e) {clearTimeout(timer);delete window[key];reject(e);}
  });
}
async function ctl(command,payload) {
  const result = JSON.parse(await exec(`sh ${quote(`${root}/control.sh`)} ${quote(command)}${payload === undefined ? '' : ` ${quote(payload)}`}`));
  if (!result.ok) throw new Error(result.error || '操作失败');
  return result;
}
function base64(text) { const bytes = new TextEncoder().encode(text);let s='';for(const b of bytes)s+=String.fromCharCode(b);return btoa(s); }
function parseConfig(text) {
  const entries={};
  for(const row of text.split('\n')) {
    const line=row.split('#')[0].trim();if(!line)continue;
    const m=line.match(/^(\w+)\s*=\s*([^=]+)$/);
    if(!m||Object.hasOwn(entries,m[1])||!Number.isFinite(Number(m[2])))throw new Error('配置格式错误或键重复');
    entries[m[1]]=m[2].trim();
  }
  return entries;
}
function syncControls(text) {
  const entries=parseConfig(text);legacyGamma=entries.manual_gamma??'2.2';
  for(const [key] of fields){const input=$(key),value=entries[key]??defaults.get(key)??'';if(input.type==='checkbox')input.checked=value==='1';else input.value=value;const range=$(`${key}_range`);if(range)range.value=value;}
}
function setConfig(text) { $('config').value=text;try{syncControls(text);rawInvalid=false;preview();}catch(e){rawInvalid=true;throw e;} }
function updateKey(key,value) {
  const lines=$('config').value.split('\n');let found=false;
  const re=new RegExp(`^\\s*${key}\\s*=`);
  for(let i=0;i<lines.length;i++)if(re.test(lines[i])){lines[i]=`${key}=${value}`;found=true;}
  if(!found)lines.push(`${key}=${value}`);
  $('config').value=lines.join('\n');dirty=true;rawInvalid=false;preview();
}
for (const [key,label,min,max,step] of fields) {
  const row=document.createElement('div');row.className='field';
  const title=document.createElement('label');title.htmlFor=key;title.textContent=label;
  const input=document.createElement('input');input.id=key;input.type=max===1&&step===1?'checkbox':'number';
  input.min=min;input.max=max;input.step=step;input.addEventListener('input',()=>{if(rawInvalid){note('请先修复原始配置格式');return;}updateKey(key,input.type==='checkbox'?(input.checked?1:0):input.value);if(input.type!=='checkbox')$(`${key}_range`).value=input.value;});row.append(title,input);
  if(input.type!=='checkbox'){const range=document.createElement('input');range.id=`${key}_range`;range.type='range';range.min=min;range.max=max;range.step=step;range.setAttribute('aria-label',label);range.addEventListener('input',()=>{if(rawInvalid)return;input.value=range.value;updateKey(key,range.value);});row.append(range);}
  $('controls').append(row);
}
$('config').addEventListener('input',()=>{dirty=true;try{syncControls($('config').value);rawInvalid=false;preview();}catch(e){rawInvalid=true;note(e.message);}});
function chart(points, current) {
  const ns='http://www.w3.org/2000/svg';$('chart').replaceChildren();
  for(let value=0;value<=100;value+=20){const line=document.createElementNS(ns,'path');line.setAttribute('d',`M30,${200-value*1.8} H580`);line.setAttribute('stroke','#ddd');$('chart').append(line);const label=document.createElementNS(ns,'text');label.setAttribute('x','0');label.setAttribute('y',200-value*1.8);label.textContent=`${value}%`;$('chart').append(label);}
  for(const lux of [0,1,10,100,1000,10000,100000]){const label=document.createElementNS(ns,'text');label.setAttribute('x',20+Math.log1p(lux)/Math.log(100001)*560);label.setAttribute('y','216');label.setAttribute('text-anchor',lux===100000?'end':'start');label.textContent=String(lux);$('chart').append(label);}
  for (const [index,color] of [[1,'#3879e5'],[2,'#ec9c32'],[3,'#af52de']]) {
    const path=document.createElementNS(ns,'path');
    path.setAttribute('d',points.map((p,i)=>`${i?'L':'M'}${20+Math.log1p(p[0])/Math.log(100001)*560},${200-Math.min(1,Math.max(0,(p[index]??p[2])))*180}`).join(' '));
    path.setAttribute('fill','none');path.setAttribute('stroke',color);path.setAttribute('stroke-width','2.5');$('chart').append(path);
  }
  if(current&&current.lux>=0&&current.current>=0) {
    const dot=document.createElementNS(ns,'circle');
    dot.setAttribute('cx',20+Math.log1p(Math.min(100000,current.lux))/Math.log(100001)*560);
    dot.setAttribute('cy',200-Math.min(1,Math.max(0,current.current))*180);
    dot.setAttribute('r','4');dot.setAttribute('fill','#b43b5d');$('chart').append(dot);
  }
}
function preview() {
  const live=runtime;
  if(!runtime?.curve) {
    const nits=(l)=>l<=.5?.222+l*1.556:l<=2?1+Math.log(l/.5)/Math.log(4)*2:l<=10?3+Math.log(l/2)/Math.log(5)*12:l<=100?15+Math.log(l/10)/Math.log(10)*85:l<=1000?100+Math.log(l/100)/Math.log(10)*200:l<=10000?300+Math.log(l/1000)/Math.log(10)*200:500+Math.log(l/10000)/Math.log(10)*38.5;
    runtime={curve:Array.from({length:81},(_,i)=>{const lux=Math.expm1(i*Math.log(100001)/80);return [lux,nits(lux)/538.5];}),maximum:1,points:[]};
  }
  try {
    const c=parseConfig($('config').value), hour=runtime.local_hour??new Date().getHours();
    const scale=(Number(c.night_enabled??1)&&(hour>=18||hour<6)) ? .95 : 1;
    const points=runtime.points??[], offset=runtime.legacy_offset??0;
    const base=(lux)=>{
      const rows=runtime.curve;let i=rows.findIndex(p=>p[0]>=lux);if(i<0)i=rows.length-1;
      if(i===0)return Math.max(0,Math.min(1,(rows[0][1]+offset)*scale));
      const a=rows[i-1],b=rows[i],t=(Math.log1p(lux)-Math.log1p(a[0]))/(Math.log1p(b[0])-Math.log1p(a[0]));
      return Math.max(0,Math.min(1,(a[1]+t*(b[1]-a[1])+offset)*scale));
    };
    const learned=(lux)=>{
      if(!points.length)return base(lux);
      const first=points[0],last=points.at(-1),left=Math.max(0,(first.lux+1)/10-1),right=Math.min(200000,(last.lux+1)*10-1);
      const knots=points.map(p=>[p.lux,p.brightness]);
      if(left<first.lux)knots.unshift([left,Math.min(base(left),first.brightness)]);
      if(right>last.lux)knots.push([right,Math.max(base(right),last.brightness)]);
      if(lux<left)return Math.min(base(lux),knots[0][1]);if(lux>right)return Math.max(base(lux),knots.at(-1)[1]);
      let i=knots.findIndex(p=>p[0]>=lux);if(i<=0)return knots[0][1];
      const a=knots[i-1],b=knots[i];return a[1]+(b[1]-a[1])*(Math.log1p(lux)-Math.log1p(a[0]))/(Math.log1p(b[0])-Math.log1p(a[0]));
    };
    const threshold=Number(c.high_lux_threshold??15000), gain=Number(c.high_lux_boost_max??.35),floor=Number(c.high_lux_min_pct??.9),cap=runtime.maximum??1;
    chart(runtime.curve.map(p=>{const normal=learned(p[0]);return [p[0],p[1]*scale,normal,p[0]<threshold?normal:Math.min(cap,Math.max(normal*(1+Math.min(1,(p[0]-threshold)/threshold)*gain),cap*floor))];}),runtime);
    $('previewState').textContent=dirty?'未保存配置预览（稳态示意，不包含过渡、保护与限温）':'已保存配置曲线（稳态示意）';
  }catch(e){note(e.message);}finally{runtime=live;if(!live)$('previewState').textContent='离线示例预览：未经当前屏幕校准，不代表实际输出';}
}
async function readConfig() {try{return (await ctl('config-get')).data;}catch{return exec(`sh ${quote(`${root}/control.sh`)} config-read`);}}
async function refresh(load=false) {
  if(polling)return;polling=true;
  try {
    const s=await ctl('status');
    runtime=s;$('version').textContent=`ROOT · ${s.version??'v7'}`;
    $('status').replaceChildren();
    for(const [label,value] of [['服务',s.bridge],['控制状态',s.ambient_guard?'位置变化保护':s.reason],['模式',s.mode],['环境光',s.lux>=0?`${s.lux.toFixed(1)} lux`:'等待传感器'],['目标',s.target===undefined?'—':`${(s.target*100).toFixed(2)}%`],['实际亮度',s.current===undefined?'—':`${(s.current*100).toFixed(2)}%`],['系统滑块值',s.slider??'—'],['处理间隔',s.poll_ms===undefined?'—':`${s.poll_ms} ms`],['光感来源',s.lux_source??'—'],['当前系统上限',s.maximum===undefined?'—':`${(s.maximum*100).toFixed(1)}%`]]) {
      const box=document.createElement('div');box.className='card';const l=document.createElement('span'),v=document.createElement('strong');l.textContent=label;v.textContent=value??'—';box.append(l,v);$('status').append(box);
    }
    $('learningState').textContent=`存储：${s.storage??'未知'} · 调节事件：${s.event_status??'等待'} · 用户序号：${s.user??'—'}`;
    $('points').replaceChildren();const table=document.createElement('table');
    for(const p of s.points??[]){const tr=document.createElement('tr');for(const value of [`${p.lux.toFixed(1)} lux`,`${(p.brightness*100).toFixed(2)}%`,new Date(p.time).toLocaleString()]){const td=document.createElement('td');td.textContent=value;tr.append(td);}table.append(tr);}$('points').append(table);
    $('diagnostics').textContent=JSON.stringify({profile:s.profile,calibration:s.calibration,learning_available:s.learning_available,window_override_detection:s.window_override_detection,sensor:s.sensor,back_sensor:s.back_sensor,proximity_sensor:s.proximity_sensor,temperature:s.temperature,battery:s.battery,observation_only:s.observation_only,config_status:s.config_status,last_event_rejection:s.last_event_rejection,events_dropped:s.events_dropped,uptime_ms:s.uptime_ms,cpu_bridge_percent:s.cpu_percent,cpu_core_percent:s.cpu_core_percent,cpu_total_percent:(s.cpu_percent??0)+(s.cpu_core_percent??0),protection:s.protection,thermal_status:s.thermal_status,brightness_max_reason:s.brightness_max_reason,learning_scope:s.learning_scope,manual_learning_armed:s.manual_learning_armed,sensor_selection:s.sensor_selection,front_lux:s.front_lux,back_lux:s.back_lux,back_lux_effective:s.back_lux_effective,back_sample_age_ms:s.back_sample_age_ms,lux_source:s.lux_source,fallback_status:s.fallback_status,poll_ms:s.poll_ms,alpha:s.alpha,fast_dark:s.fast_dark,ambient_guard:s.ambient_guard,night_scale:s.night_scale,legacy_gamma_compatibility_only:legacyGamma},null,2);
    preview();
    $('curvePoint').textContent=`红点：当前实际输出 · 照度 ${s.lux>=0?s.lux.toFixed(2):'—'} lux · 系统设置 ${s.setting>=0?(s.setting*100).toFixed(2):'—'}%`;
    if($('liveLog').checked)$('log').textContent=await exec(`sh ${quote(`${root}/control.sh`)} log-read`);
    if(load&&!dirty)setConfig(await readConfig());
    if(!busy)note(s.protection&&s.protection!=='clear'?'系统保护优先：已释放亮度控制并暂停学习。':s.ambient_guard?'前后光感变化不一致：保持亮度，暂停学习。':s.observation_only?'观察模式：记录学习，不控制屏幕。':s.learning_available?'已连接。手动调节后，请确认学习状态为 saved。':'自动事件学习不可用，可点击“学习下一次手动调节”后再拖亮度条。');
  }catch(e){$('status').replaceChildren();$('learningState').textContent='服务离线：旧状态已清除';$('points').replaceChildren();runtime=null;preview();note(e.message);
    try{if(load&&!dirty)setConfig(await readConfig());$('diagnostics').textContent=await exec(`sh ${quote(`${root}/check_status.sh`)}`);}catch(d){$('diagnostics').textContent=d.message;}
  }finally{polling=false;}
}
async function action(fn) {
  if(busy)return;busy=true;document.querySelectorAll('button').forEach(b=>b.disabled=true);
  try{await fn();}catch(e){note(e.message);}finally{busy=false;document.querySelectorAll('button').forEach(b=>b.disabled=false);}
}
async function save(text) {
  parseConfig(text);
  let online=false;try{await ctl('status');online=true;}catch{}
  if(online){await ctl('config-set',base64(text));note('核心已确认：配置已保存并应用。');}
  else {await exec(`sh ${quote(`${root}/control.sh`)} config-save ${quote(base64(text))}`);note('配置已验证并保存；服务恢复后生效。');}
  dirty=false;setConfig(online?await readConfig():await exec(`sh ${quote(`${root}/control.sh`)} config-read`));
}
$('save').onclick=()=>action(async()=>{if(rawInvalid)throw new Error('请先修复原始配置格式');for(const [key] of fields){const i=$(key);if(!i.checkValidity())throw new Error(`请检查 ${key}`);}await save($('config').value);});
$('saveRaw').onclick=()=>action(()=>save($('config').value));
$('refresh').onclick=()=>refresh(true);
$('defaults').onclick=()=>action(async()=>{dirty=true;setConfig(await exec(`cat ${quote(`${root}/default.conf`)}`));note('已载入默认值，点击保存并应用后生效。');});
$('clear').onclick=()=>{if(confirm('清除当前用户的全部学习记录？基础曲线保持不变。'))action(async()=>{await ctl('learn-clear');await refresh();note('学习记录已清除并保存。');});};
$('export').onclick=()=>action(async()=>{const data=(await ctl('learn-export')).data;const url=URL.createObjectURL(new Blob([data],{type:'text/plain'}));const a=document.createElement('a');a.href=url;a.download='ios-brightness-learning.dat';a.click();setTimeout(()=>URL.revokeObjectURL(url),10000);note('已导出当前用户学习数据。');});
$('import').onchange=()=>action(async()=>{const file=$('import').files[0];if(!file)return;if(file.size>32768)throw new Error('导入文件过大');await ctl('learn-import',base64(await file.text()));$('import').value='';await refresh();note('学习数据已验证并导入。');});
$('diagnose').onclick=()=>action(async()=>{$('diagnostics').textContent=await exec(`sh ${quote(`${root}/check_status.sh`)}`);});
$('logs').onclick=()=>action(async()=>{$('log').textContent=await exec(`sh ${quote(`${root}/control.sh`)} log-read`);});
setConfig([...defaults].map(([k,v])=>`${k}=${v}`).join('\n')+'\n');
try{const text=await exec(`cat ${quote(`${root}/default.conf`)}`);for(const [k,v] of Object.entries(parseConfig(text)))defaults.set(k,v);setConfig(text);}catch{}
await refresh(true);
setInterval(()=>{if(!document.hidden&&!busy)refresh();},3000);

$('configImport').onchange=()=>action(async()=>{
  const file=$('configImport').files[0];if(!file)return;
  if(file.size>32768)throw new Error('配置文件过大');
  const text=await file.text();$('configImport').value='';
  if(!confirm('导入并应用此配置？当前调参会被替换，学习记录保持不变。'))return;
  await save(text);await refresh();
});
$('clearLog').onclick=()=>{if(confirm('清空运行日志及轮转备份？学习记录保持不变。'))action(async()=>{await ctl('log-clear');$('log').textContent='运行日志及轮转备份已清空。';note('当前运行日志已清空，学习记录不受影响。');});};

$('learnArm').onclick=()=>action(async()=>{await ctl('learn-arm');note('已开启 30 秒学习窗口：现在轻微拖动系统亮度条，然后保持环境稳定约 3 秒。');});
$('sensorRead').onclick=()=>action(async()=>{const s=await ctl('sensors-get');$('sensorConfig').value=s.data;$('sensorList').textContent=s.sensors.join('\n');note('空值为自动选择；back=none 可关闭后置参与，sysfs 只接受照度节点。');});
$('sensorSave').onclick=()=>action(async()=>{await ctl('sensors-save',base64($('sensorConfig').value));note('光感选择已验证并保存，服务将在两秒内应用；不同来源的学习记录独立保存。');});
