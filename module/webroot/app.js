// SPDX-License-Identifier: GPL-3.0-only
const $ = (id) => document.getElementById(id);
const root = '/data/adb/modules/ios_auto_brightness';
const quote = (s) => `'${String(s).replace(/'/g, "'\\''")}'`;
let sequence = 0, busy = false, polling = false, dirty = false;
let legacyGamma = '2.2';
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
    if (!window.ksu?.exec) return reject(new Error('请在 KernelSU WebUI 中打开。'));
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
function setConfig(text) {
  $('config').value = text;
  const entries = Object.fromEntries(text.split('\n').filter(s=>/^\w+=/.test(s)).map(s=>s.split('=')));
  legacyGamma=entries.manual_gamma??'2.2';
  for (const [key] of fields) {const input=$(key);if(input.type==='checkbox')input.checked=entries[key]==='1';else input.value=entries[key]??'';}
}
for (const [key,label,min,max,step] of fields) {
  const row=document.createElement('div');row.className='field';
  const title=document.createElement('label');title.htmlFor=key;title.textContent=label;
  const input=document.createElement('input');input.id=key;input.type=max===1&&step===1?'checkbox':'number';
  input.min=min;input.max=max;input.step=step;input.addEventListener('input',()=>{dirty=true;});row.append(title,input);$('controls').append(row);
}
$('config').addEventListener('input',()=>{dirty=true;});
function chart(points, current) {
  const ns='http://www.w3.org/2000/svg';$('chart').replaceChildren();
  for (const [index,color] of [[1,'#3879e5'],[2,'#ec9c32']]) {
    const path=document.createElementNS(ns,'path');
    path.setAttribute('d',points.map((p,i)=>`${i?'L':'M'}${20+Math.log1p(p[0])/Math.log(100001)*560},${200-Math.min(1,Math.max(0,p[index]))*180}`).join(' '));
    path.setAttribute('fill','none');path.setAttribute('stroke',color);path.setAttribute('stroke-width','2.5');$('chart').append(path);
  }
  if(current&&current.lux>=0&&current.current>=0) {
    const dot=document.createElementNS(ns,'circle');
    dot.setAttribute('cx',20+Math.log1p(Math.min(100000,current.lux))/Math.log(100001)*560);
    dot.setAttribute('cy',200-Math.min(1,Math.max(0,current.current))*180);
    dot.setAttribute('r','4');dot.setAttribute('fill','#b43b5d');$('chart').append(dot);
  }
}
async function refresh(load=false) {
  if(polling)return;polling=true;
  try {
    const s=await ctl('status');
    $('version').textContent=`KERNELSU · ${s.version??'v7'}`;
    $('status').replaceChildren();
    for(const [label,value] of [['服务',s.bridge],['控制状态',s.ambient_guard?'位置变化保护':s.reason],['模式',s.mode],['环境光',s.lux>=0?`${s.lux.toFixed(1)} lux`:'等待传感器'],['目标',s.target===undefined?'—':`${(s.target*100).toFixed(2)}%`],['实际亮度',s.current===undefined?'—':`${(s.current*100).toFixed(2)}%`],['系统滑块值',s.slider??'—'],['处理间隔',s.poll_ms===undefined?'—':`${s.poll_ms} ms`],['光感来源',s.lux_source??'—'],['当前系统上限',s.maximum===undefined?'—':`${(s.maximum*100).toFixed(1)}%`]]) {
      const box=document.createElement('div');box.className='card';const l=document.createElement('span'),v=document.createElement('strong');l.textContent=label;v.textContent=value??'—';box.append(l,v);$('status').append(box);
    }
    $('learningState').textContent=`存储：${s.storage??'未知'} · 调节事件：${s.event_status??'等待'} · 用户序号：${s.user??'—'}`;
    $('points').replaceChildren();const table=document.createElement('table');
    for(const p of s.points??[]){const tr=document.createElement('tr');for(const value of [`${p.lux.toFixed(1)} lux`,`${(p.brightness*100).toFixed(2)}%`,new Date(p.time).toLocaleString()]){const td=document.createElement('td');td.textContent=value;tr.append(td);}table.append(tr);}$('points').append(table);
    $('diagnostics').textContent=JSON.stringify({profile:s.profile,sensor:s.sensor,back_sensor:s.back_sensor,proximity_sensor:s.proximity_sensor,temperature:s.temperature,battery:s.battery,observation_only:s.observation_only,config_status:s.config_status,last_event_rejection:s.last_event_rejection,events_dropped:s.events_dropped,uptime_ms:s.uptime_ms,cpu_bridge_percent:s.cpu_percent,front_lux:s.front_lux,back_lux:s.back_lux,back_lux_effective:s.back_lux_effective,back_sample_age_ms:s.back_sample_age_ms,lux_source:s.lux_source,fallback_status:s.fallback_status,poll_ms:s.poll_ms,alpha:s.alpha,fast_dark:s.fast_dark,ambient_guard:s.ambient_guard,night_scale:s.night_scale,legacy_gamma_compatibility_only:legacyGamma},null,2);
    chart(s.curve??[],s);
    $('curvePoint').textContent=`红点：当前实际输出 · 照度 ${s.lux>=0?s.lux.toFixed(2):'—'} lux · 系统设置 ${s.setting>=0?(s.setting*100).toFixed(2):'—'}%`;
    if($('liveLog').checked)$('log').textContent=await exec('tail -n 80 /data/adb/ios_auto_brightness/service.log');
    if(load&&!dirty)setConfig((await ctl('config-get')).data);
    if(!busy)note(s.ambient_guard?'前后光感变化不一致：保持亮度，暂停学习。':s.observation_only?'观察模式：记录学习，不控制屏幕。':'已连接。手动调节后，请确认学习状态为 saved。');
  }catch(e){note(e.message);}finally{polling=false;}
}
async function action(fn) {
  if(busy)return;busy=true;document.querySelectorAll('button').forEach(b=>b.disabled=true);
  try{await fn();}catch(e){note(e.message);}finally{busy=false;document.querySelectorAll('button').forEach(b=>b.disabled=false);}
}
async function save(text) {await ctl('config-set',base64(text));dirty=false;setConfig((await ctl('config-get')).data);note('核心已确认：配置已保存并应用。');}
$('save').onclick=()=>action(async()=>{const lines=fields.map(([key])=>{const i=$(key);if(!i.checkValidity())throw new Error(`请检查 ${key}`);return `${key}=${i.type==='checkbox'?(i.checked?1:0):i.value}`;});lines.push(`manual_gamma=${legacyGamma}`);await save(lines.join('\n')+'\n');});
$('saveRaw').onclick=()=>action(()=>save($('config').value));
$('refresh').onclick=()=>refresh(true);
$('defaults').onclick=()=>action(async()=>{setConfig(await exec(`cat ${quote(`${root}/default.conf`)}`));dirty=true;note('已载入默认值，点击保存并应用后生效。');});
$('clear').onclick=()=>{if(confirm('清除当前用户的全部学习记录？基础曲线保持不变。'))action(async()=>{await ctl('learn-clear');await refresh();note('学习记录已清除并保存。');});};
$('export').onclick=()=>action(async()=>{const data=(await ctl('learn-export')).data;const url=URL.createObjectURL(new Blob([data],{type:'text/plain'}));const a=document.createElement('a');a.href=url;a.download='ios-brightness-learning.dat';a.click();setTimeout(()=>URL.revokeObjectURL(url),10000);note('已导出当前用户学习数据。');});
$('import').onchange=()=>action(async()=>{const file=$('import').files[0];if(!file)return;if(file.size>32768)throw new Error('导入文件过大');await ctl('learn-import',base64(await file.text()));$('import').value='';await refresh();note('学习数据已验证并导入。');});
$('logs').onclick=()=>action(async()=>{$('log').textContent=await exec("tail -n 80 /data/adb/ios_auto_brightness/service.log");});
await refresh(true);
setInterval(()=>{if(!document.hidden&&!busy)refresh();},3000);

$('configImport').onchange=()=>action(async()=>{
  const file=$('configImport').files[0];if(!file)return;
  if(file.size>32768)throw new Error('配置文件过大');
  const text=await file.text();$('configImport').value='';
  if(!confirm('导入并应用此配置？当前调参会被替换，学习记录保持不变。'))return;
  await save(text);await refresh();note('配置已验证并应用；旧 Gamma 作为兼容信息保留。');
});
$('clearLog').onclick=()=>{if(confirm('清空当前运行日志？轮转备份会保留。'))action(async()=>{await ctl('log-clear');$('log').textContent='当前日志已清空。';note('当前运行日志已清空，学习记录不受影响。');});};
