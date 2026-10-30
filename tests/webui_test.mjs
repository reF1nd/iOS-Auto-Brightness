// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import test from 'node:test';

test('WebUI shows runtime, edits configuration, resets learning and starts the service', async () => {
  const elements = new Map();
  class Element {
    constructor(tag = 'div') { this.tag = tag; this.children = []; this.value = ''; this.checked = false; this.listeners = {}; this.attrs = {}; this.textContent = ''; }
    set id(value) { this._id = value; elements.set(value, this); }
    get id() { return this._id; }
    append(...items) { this.children.push(...items); }
    replaceChildren(...items) { this.children = items; }
    addEventListener(event, callback) { this.listeners[event] = callback; }
    setAttribute(key, value) { this.attrs[key] = value; }
  }
  const document = {
    hidden: false,
    getElementById(id) { if (!elements.has(id)) { const e = new Element(); e.id = id; } return elements.get(id); },
    createElement(tag) { return new Element(tag); },
    createElementNS(ns, tag) { return new Element(tag); },
    querySelectorAll() { return []; },
  };
  const defaults = await readFile(new URL('../module/default.conf', import.meta.url), 'utf8');
  let config = defaults.replace('manual_gamma=2.2', 'manual_gamma=2.7');
  let online = true, startCalls = 0;
  const commands = [];
  const runtime = {ok: true, version: 'v8-test', mode: 'auto', screen_on: true, proximity_near: false, front_lux: 12.5, back_lux: 300,
    lux_source: 'rear', smoothed_lux: 120, target: 2400, current: 2300, max: 16383, poll_ms: 800, sunlight: false, battery: 70,
    charging: false, slider: 42, learnable: true, last_learning: 'learned', good_curve: true,
    anchors: [[0, 7.96, 1, 142], [30, 135, 0, 1374], [600, 220, 0, 2121], [5000, 1060, 0, 6552]],
    curve: [[0.1, 142], [1, 152], [10, 391], [100, 1460]], default_curve: [[0.1, 33], [1, 47], [10, 391], [100, 1460]]};
  const verb = (command) => command.match(/control\.sh' '?([\w-]+)/)?.[1];
  const reply = (key, value) => window[key](0, typeof value === 'string' ? value : JSON.stringify(value), '');
  const window = {ksu: {exec(command, options, key) {
    commands.push(command);
    if (command.startsWith('cat ')) return reply(key, defaults);
    if (verb(command) === 'config-read') return reply(key, config);
    if (verb(command) === 'config-save') {
      config = Buffer.from(command.match(/'config-save' '([^']+)'/)[1], 'base64').toString();
      return reply(key, {ok: true, applied: online});
    }
    if (verb(command) === 'log-read') return reply(key, 'recent log');
    if (verb(command) === 'log-clear') return reply(key, {ok: true});
    if (verb(command) === 'learn-reset') { runtime.anchors = runtime.anchors.map(([l, n, , v]) => [l, n, 0, v]); return reply(key, {ok: true}); }
    if (verb(command) === 'start') { startCalls++; const state = online ? 'already_running' : 'started'; online = true; return reply(key, {ok: true, state}); }
    if (verb(command) === 'status') return reply(key, online ? runtime : {ok: false, error: '服务未运行或无响应'});
    if (command.includes('check_status.sh')) return reply(key, 'diagnostics');
    throw new Error(`Unexpected command: ${command}`);
  }}};
  const source = await readFile(new URL('../module/webroot/app.js', import.meta.url), 'utf8');
  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
  await new AsyncFunction('document', 'window', 'confirm', 'setInterval', 'btoa', source)(
    document, window, () => true, () => 0, (s) => Buffer.from(s, 'binary').toString('base64'));
  const texts = (id) => elements.get(id).children.flatMap((c) => c.children.map((e) => e.textContent));

  assert.equal(elements.get('version').textContent, 'reF1nd · v8-test');
  assert.ok(texts('status').includes('后置（系统双光感策略）'));
  assert.ok(texts('status').includes('2400 / 2300'));
  assert.ok(texts('status').includes('HyperOS 逻辑尼特曲线'));
  assert.match(elements.get('learning').textContent, /已记住 1\/3 个拖动点：0\.0 lux → 亮度条 8\.0 逻辑尼特（节点 142）/);
  assert.match(elements.get('learning').textContent, /已记住一次拖动/);
  assert.match(elements.get('learning').textContent, /息屏较久后会回落/);
  assert.equal(elements.get('chart').children.filter((e) => e.attrs.class === 'anchor').length, 1);
  assert.equal(elements.get('chart').children.at(-1).tag, 'circle');
  assert.ok(elements.get('chart').children.some((e) => e.attrs.class === 'learned'));
  assert.ok(elements.get('chart').children.some((e) => e.attrs.class === 'anchor'));
  assert.equal(elements.get('learning_enabled').type, 'checkbox');
  assert.equal(elements.get('learning_enabled').checked, true);
  assert.ok(!elements.has('slider_range'));

  elements.get('alpha_fast').value = '0.6';
  elements.get('alpha_fast').listeners.input();
  assert.match(elements.get('config').value, /alpha_fast=0.6/);
  await elements.get('save').onclick();
  assert.match(config, /alpha_fast=0\.6\n/);
  assert.match(config, /manual_gamma=2\.7\n/);
  assert.match(elements.get('notice').textContent, /已保存并立即应用/);

  elements.get('config').value = 'alpha_fast=0.7\ndual_sensor=0\n';
  elements.get('config').listeners.input();
  assert.equal(elements.get('alpha_fast_range').value, '0.7');
  assert.equal(elements.get('dual_sensor').checked, false);
  elements.get('dual_sensor').checked = true;
  elements.get('dual_sensor').listeners.input();
  await elements.get('saveRaw').onclick();
  assert.equal(config, 'alpha_fast=0.7\ndual_sensor=1\n');

  elements.get('config').value = 'alpha_fast=abc\n';
  elements.get('config').listeners.input();
  await elements.get('save').onclick();
  assert.match(elements.get('notice').textContent, /配置格式错误/);
  assert.equal(config, 'alpha_fast=0.7\ndual_sensor=1\n');

  elements.get('learnReset').onclick();
  await new Promise((resolve) => setImmediate(resolve));
  assert.ok(commands.some((c) => verb(c) === 'learn-reset'));

  elements.get('liveLog').checked = true;
  await elements.get('refresh').onclick();
  assert.equal(elements.get('log').textContent, 'recent log');

  online = false;
  await elements.get('refresh').onclick();
  assert.deepEqual(texts('status'), ['服务', '未运行']);
  assert.match(elements.get('notice').textContent, /启动服务/);
  assert.equal(startCalls, 0, 'offline polling never auto-starts');
  await elements.get('startService').onclick();
  assert.equal(startCalls, 1);
  assert.match(elements.get('notice').textContent, /服务已启动/);
  await elements.get('startService').onclick();
  assert.match(elements.get('notice').textContent, /已在运行/);
  await elements.get('diagnose').onclick();
  assert.equal(elements.get('diagnostics').textContent, 'diagnostics');
});
