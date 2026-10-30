// SPDX-License-Identifier: GPL-3.0-only
const $ = (id) => document.getElementById(id);
const root = '/data/adb/modules/ios_auto_brightness';
const quote = (s) => `'${String(s).replace(/'/g, "'\\''")}'`;
const GROUPS = [
  ['核心响应', [['alpha_fast','快速响应',0.01,0.95,0.01],['alpha_mid','中速过渡',0.01,0.95,0.01],['alpha_slow','慢速稳定',0.001,0.95,0.001],
    ['fast_threshold','快速阈值',0.05,10,0.05],['slow_threshold','稳定阈值',0.001,1,0.001]]],
  ['稳定曲线', [['spike_up','上升尖峰限幅',1,100,0.5],['spike_down','下降尖峰限幅',0.001,1,0.001],['hyst_low','低亮度回差',0,0.5,0.005],['hyst_high','高亮度回差',0,0.5,0.005]]],
  ['阳光模式', [['high_lux_threshold','触发照度（lux）',0,200000,500],['high_lux_boost_max','最高增益',0,1,0.01],['high_lux_min_pct','亮度下限比例',0,1,0.01],['high_lux_duration_ms','退出延迟（毫秒）',0,120000,500]]],
  ['学习与光感', [['learning_enabled','学习亮度条拖动',0,1,1],['dual_sensor','后置光感按系统策略参与',0,1,1]]],
  ['日志', [['log_max_bytes','日志上限（字节）',65536,8388608,65536],['log_max_backups','日志备份份数',1,8,1]]],
];
let sequence = 0, busy = false, polling = false, dirty = false, runtime = null, defaults = '';

function note(text) { $('notice').textContent = text; }
function exec(command, timeout = 10000) {
  return new Promise((resolve, reject) => {
    if (!window.ksu?.exec) return reject(new Error('请在支持 ksu.exec 的 Root 管理器 WebUI 中打开'));
    const key = `iab_${Date.now()}_${sequence++}`;
    const timer = setTimeout(() => { delete window[key]; reject(new Error('操作响应超时')); }, timeout);
    window[key] = (code, stdout, stderr) => {
      clearTimeout(timer); delete window[key];
      Number(code) === 0 ? resolve(stdout) : reject(new Error(stderr || stdout || `错误 ${code}`));
    };
    try { window.ksu.exec(command, '{}', key); } catch (e) { clearTimeout(timer); delete window[key]; reject(e); }
  });
}
async function ctl(command, payload, timeout) {
  const out = await exec(`sh ${quote(`${root}/control.sh`)} ${quote(command)}${payload === undefined ? '' : ` ${quote(payload)}`}`, timeout);
  const result = JSON.parse(out);
  if (result.ok === false) throw new Error(result.error || '操作失败');
  return result;
}
const base64 = (text) => { let s = ''; for (const b of new TextEncoder().encode(text)) s += String.fromCharCode(b); return btoa(s); };

function parseConfig(text) {
  const entries = {};
  for (const row of text.split('\n')) {
    const line = row.split('#')[0].trim();
    if (!line) continue;
    const m = line.match(/^(\w+)\s*=\s*(\S+)$/);
    if (!m || !Number.isFinite(Number(m[2]))) throw new Error(`配置格式错误：${line}`);
    entries[m[1]] = m[2];
  }
  return entries;
}
function updateKey(key, value) {
  const lines = $('config').value.split('\n'), re = new RegExp(`^\\s*${key}\\s*=`);
  let found = false;
  for (let i = 0; i < lines.length; i++) if (re.test(lines[i])) { lines[i] = `${key}=${value}`; found = true; }
  if (!found) lines.push(`${key}=${value}`);
  $('config').value = lines.join('\n');
  dirty = true;
  chart();
}
function syncControls() {
  let entries;
  try { entries = parseConfig($('config').value); } catch (e) { note(e.message); return; }
  for (const [, fields] of GROUPS) for (const [key] of fields) {
    const input = $(key), value = entries[key];
    if (value === undefined) continue;
    if (input.type === 'checkbox') input.checked = Number(value) !== 0;
    else { input.value = value; $(`${key}_range`).value = value; }
  }
}
function buildControls() {
  for (const [title, fields] of GROUPS) {
    const heading = document.createElement('h3'); heading.textContent = title; $('controls').append(heading);
    for (const [key, label, min, max, step] of fields) {
      const row = document.createElement('div'); row.className = 'field';
      const name = document.createElement('label'); name.htmlFor = key; name.textContent = label;
      const input = document.createElement('input'); input.id = key;
      input.type = max === 1 && step === 1 ? 'checkbox' : 'number';
      Object.assign(input, {min, max, step});
      input.addEventListener('input', () => {
        updateKey(key, input.type === 'checkbox' ? (input.checked ? 1 : 0) : input.value);
        if (input.type !== 'checkbox') $(`${key}_range`).value = input.value;
      });
      row.append(name, input);
      if (input.type !== 'checkbox') {
        const range = document.createElement('input'); range.id = `${key}_range`; range.type = 'range';
        Object.assign(range, {min, max, step}); range.setAttribute('aria-label', label);
        range.addEventListener('input', () => { input.value = range.value; updateKey(key, range.value); });
        row.append(range);
      }
      $('controls').append(row);
    }
  }
}

function chart() {
  const ns = 'http://www.w3.org/2000/svg', svg = $('chart');
  svg.replaceChildren();
  const max = runtime?.max > 0 ? runtime.max : 16383;
  const x = (lux) => 40 + (Math.log10(Math.max(0.1, lux)) + 1) / 6 * 550, y = (level) => 205 - level / max * 190;
  const add = (tag, attrs, text) => { const e = document.createElementNS(ns, tag); for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); if (text !== undefined) e.textContent = text; svg.append(e); };
  for (let p = 0; p <= 100; p += 25) { add('path', {d: `M40,${y(max * p / 100)} H590`, class: 'grid'}); add('text', {x: 0, y: y(max * p / 100) + 4}, `${p}%`); }
  for (const lux of [0.1, 1, 10, 100, 1000, 10000, 100000]) add('text', {x: x(lux), y: 224, 'text-anchor': 'middle'}, lux >= 1000 ? `${lux / 1000}k` : String(lux));
  // Both curves come from the daemon: they depend on this ROM's display and framework config.
  const path = (points) => points.map(([lux, level], i) => `${i ? 'L' : 'M'}${x(lux).toFixed(1)},${y(Math.min(max, level)).toFixed(1)}`).join(' ');
  const base = runtime?.ok ? runtime.default_curve ?? [] : [], current = runtime?.ok ? runtime.curve ?? [] : [];
  if (base.length > 1) add('path', {d: path(base), class: 'base'});
  if (current.length > 1 && current.some(([, level], i) => level !== base[i]?.[1])) add('path', {d: path(current), class: 'learned'});
  for (const [lux, , user, level] of runtime?.ok ? runtime.anchors ?? [] : []) if (user) add('circle', {cx: x(lux), cy: y(Math.min(max, level)), r: 4, class: 'anchor'});
  if (runtime?.ok && runtime.current > 0) add('circle', {cx: x(runtime.smoothed_lux), cy: y(Math.min(max, runtime.current)), r: 5, class: 'point'});
  $('chartNote').textContent = runtime?.ok ? `当前 ${runtime.smoothed_lux.toFixed(1)} lux → 目标 ${runtime.target} / 输出 ${runtime.current}（最大 ${max}）` : '服务离线时无法显示曲线（曲线来自本机系统配置）';
}

function card(label, value) {
  const box = document.createElement('div'); box.className = 'card';
  const l = document.createElement('span'), v = document.createElement('strong');
  l.textContent = label; v.textContent = value ?? '—'; box.append(l, v); return box;
}
const lux = (v) => v >= 0 ? `${v.toFixed(1)} lux` : '—';
async function refresh(loadConfig = false, announce = false) {
  if (polling) return; polling = true;
  try {
    const s = await ctl('status');
    runtime = s;
    $('version').textContent = `reF1nd · ${s.version}`;
    const source = {front: '前置', rear: '后置（系统双光感策略）', back: '后置（前置无读数）', fallback: '备用读取'}[s.lux_source] ?? s.lux_source;
    $('status').replaceChildren(
      card('服务', '运行中'), card('模式', s.mode === 'auto' ? '自动亮度' : '手动（已交还系统）'),
      card('屏幕', s.screen_on ? (s.proximity_near ? '亮屏 · 接近遮挡' : '亮屏') : '熄屏'),
      card('前置 / 后置', `${lux(s.front_lux)} / ${lux(s.back_lux)}`), card('调光依据', source),
      card('平滑照度', lux(s.smoothed_lux)), card('目标 / 输出', `${s.target} / ${s.current}`),
      card('处理间隔', `${s.poll_ms} ms`), card('阳光模式', s.sunlight ? '开启' : '关闭'),
      card('电量', `${s.battery}%${s.charging ? ' · 充电中' : ''}`), card('亮度条', String(s.slider)),
      card('亮度曲线', s.learnable ? 'HyperOS 逻辑尼特曲线' : '系统默认曲线（不学习）'));
    const last = {none: '尚未触发', learned: '已记住一次拖动', disabled: '学习已关闭', reset: '已清除',
      unsupported: '本机曲线不支持学习', good_curve: '息屏后已回落到系统建议亮度'}[s.last_learning] ?? s.last_learning;
    const anchors = (s.anchors ?? []).filter(([, , user]) => user).map(([at, logical, , level]) => `${at.toFixed(1)} lux → 亮度条 ${logical.toFixed(1)} 逻辑尼特（节点 ${level}）`);
    const good = s.good_curve ? ' · 最近一次拖动高于系统建议亮度，息屏较久后会回落' : '';
    $('learning').textContent = s.learnable
      ? `已记住 ${anchors.length}/3 个拖动点${anchors.length ? `：${anchors.join('；')}` : ''} · 最近一次：${last}${good}`
      : `本机系统曲线不是 HyperOS 逻辑尼特曲线，拖动不会被学习 · 最近一次：${last}`;
    if (!busy || announce) note(s.mode === 'auto' ? '已连接，正在按系统亮度曲线调光。' : '系统处于手动亮度，模块已交还背光控制。');
  } catch (e) {
    runtime = null;
    $('status').replaceChildren(card('服务', '未运行'));
    $('learning').textContent = '服务离线';
    if (!busy || announce) note(`${e.message}。如刚在管理器中重新启用模块，请点击“启动服务”。`);
  } finally {
    chart();
    if (loadConfig && !dirty) {
      try { $('config').value = await exec(`sh ${quote(`${root}/control.sh`)} config-read`); syncControls(); } catch (e) { note(e.message); }
    }
    if ($('liveLog').checked) { try { $('log').textContent = await exec(`sh ${quote(`${root}/control.sh`)} log-read`); } catch {} }
    polling = false;
  }
}
async function action(fn) {
  if (busy) return; busy = true;
  document.querySelectorAll('button').forEach((b) => { b.disabled = true; });
  try { await fn(); } catch (e) { note(e.message); } finally {
    busy = false; document.querySelectorAll('button').forEach((b) => { b.disabled = false; });
  }
}
async function save() {
  parseConfig($('config').value);
  const result = await ctl('config-save', base64($('config').value));
  dirty = false;
  note(result.applied ? '配置已保存并立即应用。' : '配置已保存，服务启动后生效。');
  await refresh(true);
}

buildControls();
$('config').addEventListener('input', () => { dirty = true; syncControls(); chart(); });
$('refresh').onclick = () => action(() => refresh(true, true));
$('startService').onclick = () => action(async () => {
  note('正在启动服务并确认就绪……');
  const result = await ctl('start', undefined, 60000);
  await refresh(true);
  note(result.state === 'already_running' ? '服务已在运行。' : '服务已启动。');
});
$('save').onclick = () => action(save);
$('saveRaw').onclick = () => action(save);
$('defaults').onclick = () => action(async () => {
  $('config').value = defaults || await exec(`cat ${quote(`${root}/default.conf`)}`);
  dirty = true; syncControls(); chart();
  note('已载入默认设置，点击“保存并应用”后生效。');
});
$('learnReset').onclick = () => { if (confirm('清除学习记录？亮度曲线回到系统默认。')) action(async () => { await ctl('learn-reset'); await refresh(); note('学习记录已清除。'); }); };
$('logs').onclick = () => action(async () => { $('log').textContent = await exec(`sh ${quote(`${root}/control.sh`)} log-read`); });
$('clearLog').onclick = () => { if (confirm('清空运行日志及轮转备份？')) action(async () => { await ctl('log-clear'); $('log').textContent = ''; note('日志已清空。'); }); };
$('diagnose').onclick = () => action(async () => { $('diagnostics').textContent = await exec(`sh ${quote(`${root}/check_status.sh`)}`, 20000); });
try { defaults = await exec(`cat ${quote(`${root}/default.conf`)}`); } catch {}
await refresh(true);
setInterval(() => { if (!document.hidden && !busy) refresh(); }, 3000);
