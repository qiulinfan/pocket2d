import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { compileProject } from './compile.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const args=process.argv.slice(2);
if(args.length!==4||args[0]!=='--runtime'||args[2]!=='--python'){
 console.error('node replay.mjs --runtime GAME_BINARY --python PYTHON');process.exitCode=1;
}else{
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'pocket-tstl-replay-'));
 try{
  const output=path.join(temporary,'generated');
  const result=compileProject(path.join(root,'tests/script_authoring/tsconfig.json'),output);
  if(!result.ok){console.error(JSON.stringify(result));process.exitCode=1;}
  else{
   const run=spawnSync(args[3],[path.join(root,'tests/agent_typed_gameplay_test.py'),path.resolve(args[1]),path.join(root,'tests/fixtures/typed_gameplay'),output],{encoding:'utf8',maxBuffer:8*1024*1024,timeout:80000});
   process.stdout.write(run.stdout??'');process.stderr.write(run.stderr??'');
   if(run.error)console.error(run.error.message);
   process.exitCode=run.status===0?0:1;
  }
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
