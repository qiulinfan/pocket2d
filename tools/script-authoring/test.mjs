import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { compileProject } from './compile.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'pocket-tstl-check-'));
const output=path.join(temp,'generated'), project=path.join(temp,'tsconfig.json'), source=path.join(temp,'probe.ts');
const config={compilerOptions:{target:'ES2020',lib:['ES2020'],module:'ESNext',moduleResolution:'Bundler',strict:true,types:[],rootDir:temp,noEmitOnError:true},tstl:{luaTarget:'5.4'},files:[path.join(root,'types/pocket2d.d.ts'),source]};
let checks=0;
function snapshot(dir){const result={};if(!fs.existsSync(dir))return result;for(const name of fs.readdirSync(dir).sort()){const file=path.join(dir,name);if(fs.statSync(file).isDirectory()){result[name+'/']='<directory>';for(const [key,value]of Object.entries(snapshot(file)))result[name+'/'+key]=value;}else result[name]=fs.readFileSync(file).toString('base64');}return result;}
function rejected(code,expected){fs.writeFileSync(source,code);const before=snapshot(output);const result=compileProject(project,output);assert(!result.ok,'unexpected compile success');assert.deepEqual(snapshot(output),before);assert(result.diagnostics.some(d=>d.category==='error'&&d.code===expected),JSON.stringify(result));assert(result.diagnostics.some(d=>d.file===source&&d.line>0&&d.column>0));++checks;}
const valid=`declare let Probe:{value:number}; Probe={value:1};
const actor=Actor.Find("player");if(actor){const body=actor.GetComponent("Rigidbody");if(body)body.SetVelocity(Vector2(1,2));}
const saved=Save.Read("main");if(saved.ok&&typeof saved.data==="object")Debug.Log("loaded");`;
try{
 fs.writeFileSync(project,JSON.stringify(config));fs.writeFileSync(source,valid);
 let result=compileProject(project,output);assert(result.ok,JSON.stringify(result));
 const lua=fs.readFileSync(path.join(output,'probe.lua'),'utf8');
 for(const pattern of [/Probe =/,/Actor\.Find\(/,/body:SetVelocity\(/,/Vector2\(1, 2\)/,/Save\.Read\(/])assert.match(lua,pattern);
 assert(fs.existsSync(path.join(output,'TSTL-LICENSE.txt')));++checks;
 rejected('Input.GetKey(42);',2345);
 rejected('const actor=Actor.Find("missing");actor.GetName();',18048);
 rejected('declare const body:Rigidbody;body.SetVelocity({x:1,y:2,Normalize(){return 0;},Length(){return 1;}});',2345);
 rejected('declare const animation:SpriteAnimation;animation.frame=2;',2540);
 rejected('declare const body:Rigidbody;const detached=body.SetVelocity;detached(Vector2(1,2));',2684);
 rejected('declare const actor:Actor;actor.NotARealMethod();',2339);
 rejected('declare const actor:Actor;actor.GetUID();',2339);
 rejected('const saved=Save.Read("main");if(saved.ok)saved.data.health;',18046);
 rejected('declare const bag:InventoryContainer;declare const fake:Omit<InventoryContainer,"__pocketNativeInventoryContainer">;bag.Move(1,fake,1);',2345);
 rejected('const slot:InventorySlot={item_id:"wood",count:"2",instance_id:""};',2322);
 rejected('declare const item:ItemDefinition;item.attack="high";',2322);
 rejected('declare const bag:InventoryContainer;const detached=bag.Remove;detached(1,1);',2684);
 rejected('declare const catalog:ItemCatalog;declare const bag:InventoryContainer;const wrong={Remove:bag.Remove,catalog};wrong.Remove(1,1);',2684);
 rejected('declare const bag:InventoryContainer;bag.Move(1,bag,2,undefined,"yes");',2345);
 rejected('declare const catalog:ItemCatalog;catalog.GetItem("missing").display_name;',2532);
 fs.writeFileSync(source,fs.readFileSync(path.join(root,'tests/script_authoring/InventoryProbe.ts'),'utf8'));
 result=compileProject(project,output);assert(result.ok,JSON.stringify(result));
 const inventoryLua=fs.readFileSync(path.join(output,'probe.lua'),'utf8');
 for(const pattern of [/Inventory\.Open\(/,/catalog:Create\(/,/catalog:GetItem\(/,/bag:GetSlot\(1\)/,/bag:Snapshot\(/,/bag:Add\(/,/bag:Remove\(/,/bag:Move\(/,/bag:Split\(/,/bag:TransferAll\(/,/Inventory\.RestoreGroup\(/])assert.match(inventoryLua,pattern);
 assert.match(inventoryLua,/chest:Move\(\s*1,\s*bag,\s*1,\s*nil,\s*false\s*\)/);++checks;
 // Keep the preexisting promotion tests' baseline tied to their own probe.
 fs.writeFileSync(source,valid);result=compileProject(project,output);assert(result.ok,JSON.stringify(result));
 let before=snapshot(output);fs.writeFileSync(source,'const unsupported=1n;');result=compileProject(project,output);
 assert(!result.ok&&result.diagnostics.some(d=>d.category==='error'&&/BigInt|bigint|unsupported/i.test(d.message)),JSON.stringify(result));assert.deepEqual(snapshot(output),before);++checks;
 fs.writeFileSync(source,valid.replace('value:1','value:2'));
 const rename=fs.renameSync;let injected=false;
 fs.renameSync=(from,to)=>{if(!injected&&path.basename(from).startsWith('.pocket-tstl-stage-')&&to===output){injected=true;throw Object.assign(new Error('injected promotion failure'),{code:'EACCES'});}return rename(from,to);};
 try{result=compileProject(project,output);}finally{fs.renameSync=rename;}
 assert(injected&&!result.ok);assert.deepEqual(snapshot(output),before);++checks;
 result=compileProject(project,output);assert(result.ok);assert.match(fs.readFileSync(path.join(output,'probe.lua'),'utf8'),/value = 2/);++checks;
 const unmanaged=path.join(temp,'user-work');fs.mkdirSync(path.join(unmanaged,'empty-user-directory'),{recursive:true});before=snapshot(unmanaged);result=compileProject(project,unmanaged);
 assert(!result.ok&&result.diagnostics.some(d=>d.code==='unowned_output'));assert.deepEqual(snapshot(unmanaged),before);++checks;
 fs.writeFileSync(path.join(output,'notes.txt'),'user work');before=snapshot(output);result=compileProject(project,output);assert(!result.ok&&result.diagnostics.some(d=>d.code==='unmanaged_output'));assert.deepEqual(snapshot(output),before);fs.unlinkSync(path.join(output,'notes.txt'));++checks;
 const write=fs.writeFileSync;let inserted=false;before=snapshot(output);
 fs.writeFileSync=(file,...args)=>{const answer=write(file,...args);if(!inserted&&String(file).includes('.pocket-tstl-stage-')&&String(file).endsWith('probe.lua')){inserted=true;write(path.join(output,'late-notes.txt'),'concurrent user work');}return answer;};
 try{result=compileProject(project,output);}finally{fs.writeFileSync=write;}
 assert(inserted&&!result.ok&&result.diagnostics.some(d=>d.code==='unmanaged_output'));assert.equal(fs.readFileSync(path.join(output,'late-notes.txt'),'utf8'),'concurrent user work');fs.unlinkSync(path.join(output,'late-notes.txt'));assert.deepEqual(snapshot(output),before);++checks;
 const lock=path.join(temp,'.generated.pocket-tstl-lock');fs.mkdirSync(lock);try{result=compileProject(project,output);assert(!result.ok&&result.diagnostics.some(d=>d.code==='output_busy'));}finally{fs.rmdirSync(lock);}++checks;
 fs.writeFileSync(project,JSON.stringify({...config,compilerOptions:{...config.compilerOptions,noEmit:true}}));before=snapshot(output);result=compileProject(project,output);assert(!result.ok);assert.deepEqual(snapshot(output),before);++checks;
 fs.writeFileSync(path.join(temp,'helper.ts'),'export const answer=42;');fs.writeFileSync(source,'import {answer} from "./helper";declare let Probe:{value:number};Probe={value:answer};');
 fs.writeFileSync(project,JSON.stringify({...config,compilerOptions:{...config.compilerOptions,traceResolution:true,listFiles:true,listEmittedFiles:true,diagnostics:true,extendedDiagnostics:true}}));
 const cli=spawnSync(process.execPath,[path.join(root,'tools/script-authoring/compile.mjs'),'--project',project,'--out-dir',output],{encoding:'utf8'});assert.equal(cli.status,0,cli.stderr+cli.stdout);assert.equal(JSON.parse(cli.stdout).ok,true);++checks;
 result=compileProject(path.join(root,'tests/script_authoring/tsconfig.json'),path.join(temp,'gameplay'));assert(result.ok,JSON.stringify(result));assert(fs.existsSync(path.join(temp,'gameplay','TypedController.lua')));++checks;
 console.log(`PASS ${checks} typed compiler, real diagnostics, atomic promotion and output ownership checks`);
}finally{fs.rmSync(temp,{recursive:true,force:true});}
