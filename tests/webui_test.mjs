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
  let config=await readFile(new URL('../module/default.conf',import.meta.url),'utf8');
  config=config.replace('manual_gamma=2.2','manual_gamma=2.7');
  const commands=[];
  const runtime={ok:true,version:'v7-test',bridge:'active',reason:'auto',mode:'auto',lux:100,current:.1,target:.11,setting:.12,slider:'812',maximum:.4,poll_ms:8000,lux_source:'front',points:[],curve:[[0,0,0],[100,.1,.11]]};
  const window={ksu:{exec(command,options,key){
    commands.push(command);let output;
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
  assert.equal(elements.get('version').textContent,'KERNELSU · v7-test');
  assert.ok(elements.get('status').children.some(c=>c.children.some(e=>e.textContent==='812')));
  assert.equal(elements.get('chart').children.at(-1).tag,'circle');
  assert.equal(elements.get('night_enabled').type,'checkbox');
  elements.get('alpha_fast').value='0.6';
  await elements.get('save').onclick();
  assert.match(config,/alpha_fast=0.6\n/);assert.match(config,/manual_gamma=2.7\n/);
  elements.get('configImport').files=[{size:60,async text(){return 'alpha_fast=0.5\nmanual_gamma=2.4\n';}}];
  await elements.get('configImport').onchange();
  assert.match(config,/manual_gamma=2.4/);
  elements.get('liveLog').checked=true;
  await elements.get('refresh').onclick();assert.equal(elements.get('log').textContent,'recent log');
  elements.get('clearLog').onclick();
  await new Promise(resolve=>setImmediate(resolve));
  assert.ok(commands.some(c=>c.includes("'log-clear'")));
  assert.ok(!commands.some(c=>c.includes("'learn-clear'")));
});
