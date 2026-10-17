// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import test from 'node:test';

test('WebUI preserves imported tuning and exposes runtime and log controls',async()=>{
  const elements=new Map();
  class Element {
    constructor(tag='div'){this.tag=tag;this.children=[];this.value='';this.checked=false;this.files=[];this.listeners={};this.attrs={};}
    set id(value){this._id=value;elements.set(value,this);} get id(){return this._id;}
    append(...items){this.children.push(...items);} replaceChildren(){this.children=[];}
    addEventListener(event,callback){this.listeners[event]=callback;}
    setAttribute(key,value){this.attrs[key]=value;} checkValidity(){return true;} click(){}
  }
  const document={hidden:false,getElementById(id){if(!elements.has(id)){const e=new Element();e.id=id;}return elements.get(id);},
    createElement(tag){return new Element(tag);},createElementNS(ns,tag){return new Element(tag);},querySelectorAll(){return [];}};
  let online=true, logText='recent log', startError='';
  let config=await readFile(new URL('../module/default.conf',import.meta.url),'utf8');
  config=config.replace('manual_gamma=2.2','manual_gamma=2.7');
  const commands=[];
  const runtime={ok:true,version:'v7-test',bridge:'active',reason:'auto',mode:'auto',lux:100,current:.1,target:.11,setting:.12,slider:'812',maximum:.4,poll_ms:8000,lux_source:'front',points:[],curve:[[0,0,0],[100,.1,.11]]};
  const window={ksu:{exec(command,options,key){
    commands.push(command);let output;
    if(command.includes('config-save')){const match=command.match(/config-save '([^']+)'/);config=Buffer.from(match[1],'base64').toString();window[key](0,'','');return;}
    if(command.includes('config-read')){window[key](0,config,'');return;}
    if(command.includes('check_status.sh')){window[key](0,'OFFLINE: stale snapshot','');return;}
    if(command.startsWith('cat ')){window[key](0,config,'');return;}
    if(command.includes('log-read')){window[key](0,logText,'');return;}
    if(command.includes("'backlight'")){window[key](0,JSON.stringify({ok:true,nodes:[{path:'/sys/class/backlight/main',requested:200,actual:120,maximum:1000}]}),'');return;}
    if(command.includes("'log-clear'")){window[key](0,'{"ok":true}','');return;}
    if(command.includes("'sensors-get'")){window[key](0,JSON.stringify({ok:true,data:'front=\nback=\nsysfs=\n',sensors:['Exact Light']}),'');return;}
    if(command.includes("'sensors-save'")){window[key](0,'{"ok":true}','');return;}
    if(command.includes("'start'")){if(startError){window[key](0,JSON.stringify({ok:false,error:startError}),'');return;}const state=online?'already_running':'started';online=true;window[key](0,JSON.stringify({ok:true,state}),'');return;}
    if(!online){window[key](1,'','service unavailable');return;}
    if(command.includes("'learn-arm'")){window[key](0,'{"ok":true}','');return;}
    if(command.includes("'config-set'")) {
      const match=command.match(/'config-set' '([^']+)'/);assert.ok(match);
      config=Buffer.from(match[1],'base64').toString();output={ok:true};
    }else if(command.includes("'config-get'"))output={ok:true,data:config};
    else if(command.includes("'log-clear'"))output={ok:true};
    else if(command.includes("'status'"))output=runtime;
    else if(command.startsWith('tail ')){window[key](0,'recent log','');return;}
    else throw new Error('Unexpected command: '+command);
    window[key](0,JSON.stringify(output),'');
  }}};
  const source=await readFile(new URL('../module/webroot/app.js',import.meta.url),'utf8');
  const AsyncFunction=Object.getPrototypeOf(async function(){}).constructor;
  await new AsyncFunction('document','window','confirm','setInterval',source)(document,window,()=>true,()=>0);
  assert.equal(elements.get('version').textContent,'reF1nd · v7-test');
  assert.ok(elements.get('status').children.some(c=>c.children.some(e=>e.textContent==='812')));
  assert.equal(elements.get('chart').children.at(-1).tag,'circle');
  assert.equal(elements.get('night_enabled').type,'checkbox');
  assert.equal(elements.get('settings_learning').checked,false);
  assert.match(elements.get('backlight').textContent,/请求值 200 · 驱动回报 120/);
  assert.ok(elements.get('status').children.some(c=>c.children.some(e=>e.textContent==='框架报告亮度')));
  elements.get('alpha_fast').value='0.6';
  elements.get('alpha_fast').listeners.input();
  assert.match(elements.get('config').value,/alpha_fast=0.6/);
  await elements.get('save').onclick();
  assert.match(config,/alpha_fast=0.6\n/);assert.match(config,/manual_gamma=2.7\n/);
  elements.get('configImport').files=[{size:60,async text(){return 'alpha_fast=0.5\nmanual_gamma=2.4\n';}}];
  await elements.get('configImport').onchange();
  assert.match(config,/manual_gamma=2.4/);
  elements.get('config').value='alpha_fast=0.7\nmanual_gamma=2.9\n';elements.get('config').listeners.input();
  assert.equal(elements.get('alpha_fast').value,'0.7');
  assert.equal(elements.get('alpha_fast_range').value,'0.7');
  elements.get('alpha_fast_range').value='0.8';elements.get('alpha_fast_range').listeners.input();
  await elements.get('saveRaw').onclick();assert.match(config,/alpha_fast=0.8/);assert.match(config,/manual_gamma=2.9/);
  elements.get('high_lux_min_pct').value='0.3';elements.get('high_lux_min_pct').listeners.input();
  assert.match(elements.get('previewState').textContent,/未保存/);
  assert.ok(elements.get('chart').children.some(e=>e.attrs.stroke==='#af52de'));
  const oldCurve=runtime.curve,oldCap=runtime.maximum;
  runtime.curve=[[15000,.1,.1],[22500,.1,.1],[30000,.1,.1]];runtime.maximum=1;runtime.local_hour=12;
  await elements.get('refresh').onclick();
  elements.get('high_lux_min_pct').value='0.9';elements.get('high_lux_min_pct').listeners.input();
  const solar=elements.get('chart').children.find(e=>e.attrs.stroke==='#af52de');
  const solarY=solar.attrs.d.split(' ').map(p=>Number(p.split(',')[1]));
  for(const [i,expected] of [182,110,38].entries())assert.ok(Math.abs(solarY[i]-expected)<1e-8,'solar preview rises continuously from threshold');
  runtime.curve=oldCurve;runtime.maximum=oldCap;
  elements.get('liveLog').checked=true;
  await elements.get('refresh').onclick();assert.equal(elements.get('log').textContent,'recent log');
  elements.get('clearLog').onclick();
  await new Promise(resolve=>setImmediate(resolve));
  assert.ok(commands.some(c=>c.includes("'log-clear'")));
  assert.ok(!commands.some(c=>c.includes("'learn-clear'")));
  await elements.get('learnArm').onclick();assert.match(elements.get('notice').textContent,/30 秒/);
  online=false;
  await elements.get('sensorRead').onclick();assert.equal(elements.get('sensorList').textContent,'Exact Light');
  await elements.get('sensorSave').onclick();assert.match(elements.get('notice').textContent,/独立保存/);
  await elements.get('logs').onclick();assert.equal(elements.get('log').textContent,'recent log');
  elements.get('clearLog').onclick();await new Promise(resolve=>setImmediate(resolve));
  assert.match(elements.get('log').textContent,/备份已清空/);
  logText='new offline log';
  await elements.get('refresh').onclick();assert.equal(elements.get('status').children.length,0);
  assert.equal(elements.get('log').textContent,'new offline log');
  assert.match(elements.get('backlight').textContent,/驱动回报 120/);
  assert.match(elements.get('diagnostics').textContent,/OFFLINE/);
  elements.get('config').value='alpha_fast=0.5\n';elements.get('config').listeners.input();
  await elements.get('saveRaw').onclick();assert.equal(config,'alpha_fast=0.5\n');
  assert.match(elements.get('notice').textContent,/服务恢复后生效/);
  const starts=()=>commands.filter(c=>c.includes("'start'")).length;
  assert.equal(starts(),0,'offline polling must never auto-start');
  startError='模块仍被禁用，请先启用';
  await elements.get('startService').onclick();
  assert.match(elements.get('notice').textContent,/仍被禁用/);
  assert.equal(online,false);
  startError='';
  await elements.get('startService').onclick();
  assert.match(elements.get('notice').textContent,/服务已启动/);
  assert.ok(elements.get('status').children.length>0);
  await elements.get('startService').onclick();
  assert.match(elements.get('notice').textContent,/无需重复启动/);
  runtime.output_fault='driver_output_mismatch';
  await elements.get('refresh').onclick();
  assert.match(elements.get('notice').textContent,/持续异常停止/);
  assert.match(elements.get('notice').textContent,/禁用再启用/);
  assert.match(elements.get('diagnostics').textContent,/driver_output_mismatch/);
  assert.equal(starts(),3,'displaying a latched fault never retries output');
  delete runtime.output_fault;
  elements.get('config').value='alpha_fast=0.5\nalpha_fast=0.6';elements.get('config').listeners.input();
  await elements.get('save').onclick();assert.equal(config,'alpha_fast=0.5\n');

});
