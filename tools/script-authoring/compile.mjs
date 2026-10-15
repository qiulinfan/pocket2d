import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import ts from 'typescript';
import tstl from 'typescript-to-lua';

const require = createRequire(import.meta.url);
const toolRoot = path.dirname(fileURLToPath(import.meta.url));
const packageSpec = JSON.parse(fs.readFileSync(path.join(toolRoot, 'package.json'), 'utf8'));
const markerName = '.pocket-tstl-output.json';
class ToolError extends Error {
    constructor(code, message) { super(message); this.code = code; }
}
function toolDiagnostic(error) {
    return { category: 'error', code: error.code ?? 'tool_error', message: error.message ?? String(error) };
}
function diagnostic(value) {
    const result = { category: ts.DiagnosticCategory[value.category].toLowerCase(), code: value.code,
        message: ts.flattenDiagnosticMessageText(value.messageText, '\n') };
    if (value.file) {
        result.file = path.resolve(value.file.fileName);
        if (value.start !== undefined) {
            const position = value.file.getLineAndCharacterOfPosition(value.start);
            result.line = position.line + 1; result.column = position.character + 1;
        }
    }
    return result;
}
function filesWithin(directory, prefix = '', directories = []) {
    const result = [];
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
        const relative = path.join(prefix, entry.name);
        if (entry.isSymbolicLink()) throw new ToolError('unsafe_output', 'Generated output must not contain symbolic links');
        if (entry.isDirectory()) {
            directories.push(relative.split(path.sep).join('/'));
            result.push(...filesWithin(path.join(directory, entry.name), relative, directories));
        }
        else if (entry.isFile()) result.push(relative.split(path.sep).join('/'));
        else throw new ToolError('unsafe_output', 'Generated output contains a non-regular file');
    }
    return result.sort();
}
function validateOutput(directory) {
    if (!fs.existsSync(directory)) return;
    if (fs.lstatSync(directory).isSymbolicLink() || !fs.statSync(directory).isDirectory())
        throw new ToolError('unsafe_output', 'Output must be a dedicated directory, not a link or file');
    const directories = [];
    const files = filesWithin(directory, "", directories);
    if (files.length === 0 && directories.length === 0) return;
    const marker = path.join(directory, markerName);
    if (!fs.existsSync(marker)) throw new ToolError('unowned_output', 'Refusing to replace a nonempty directory not owned by Pocket2D TSTL');
    let ownership;
    try { ownership = JSON.parse(fs.readFileSync(marker, 'utf8')); }
    catch { throw new ToolError('invalid_output_marker', 'Generated-output ownership marker is invalid'); }
    if (ownership.generator !== 'pocket2d-tstl' || ownership.format !== 1 || !Array.isArray(ownership.files) ||
        ownership.files.some(value => typeof value !== 'string'))
        throw new ToolError('invalid_output_marker', 'Generated-output ownership marker is invalid');
    const expectedDirectories = new Set();
    for (const file of ownership.files) {
        for (let parent = path.posix.dirname(file); parent !== '.'; parent = path.posix.dirname(parent)) {
            if (parent === '/' || parent.startsWith('..')) throw new ToolError('invalid_output_marker', 'Invalid output path');
            expectedDirectories.add(parent);
        }
    }
    if (JSON.stringify([...expectedDirectories].sort()) !== JSON.stringify(directories.sort()))
        throw new ToolError('unmanaged_output', 'Output contains unmanaged directories');
    const expected = [...ownership.files, markerName].sort();
    if (JSON.stringify(expected) !== JSON.stringify(files))
        throw new ToolError('unmanaged_output', 'Output contains missing or unmanaged files; preserve them before choosing a clean output directory');
}
function verifyToolchain() {
    if (Number(process.versions.node.split('.')[0]) < 20)
        throw new ToolError('node_version', 'Pocket2D authoring requires Node.js 20 or newer');
    const versions = {
        typescript: ts.version,
        'typescript-to-lua': JSON.parse(fs.readFileSync(require.resolve('typescript-to-lua/package.json'), 'utf8')).version,
    };
    for (const [name, version] of Object.entries(versions))
        if (packageSpec.devDependencies[name] !== version)
            throw new ToolError('toolchain_mismatch', `${name} ${version} does not match the pinned dependency; run npm ci --ignore-scripts`);
    return versions;
}
export function compileProject(projectFile, outputDirectory) {
    let stage, backup, lock;
    const emitted = [];
    const diagnostics = [];
    let versions;
    try {
        versions = verifyToolchain();
        const project = path.resolve(projectFile);
        const output = path.resolve(outputDirectory);
        if (!fs.statSync(project).isFile()) throw new ToolError('invalid_project', 'Project must be a tsconfig file');
        const parent = path.dirname(output);
        if (parent === output) throw new ToolError('unsafe_output', 'Filesystem root cannot be a compiler output');
        fs.mkdirSync(parent, { recursive: true });
        const lockPath = path.join(parent, `.${path.basename(output)}.pocket-tstl-lock`);
        try { fs.mkdirSync(lockPath); lock = lockPath; }
        catch (error) {
            if (error.code === 'EEXIST') throw new ToolError('output_busy', 'Output has a compiler lock; do not remove it while another compiler is running');
            throw error;
        }
        validateOutput(output);
        stage = fs.mkdtempSync(path.join(parent, '.pocket-tstl-stage-'));
        const parsed = tstl.parseConfigFileWithSystem(project);
        diagnostics.push(...parsed.errors.map(diagnostic));
        if (parsed.errors.some(value => value.category === ts.DiagnosticCategory.Error)) return { ok: false, diagnostics, emitted: [] };
        if (parsed.options.luaTarget && parsed.options.luaTarget !== tstl.LuaTarget.Lua54)
            throw new ToolError('unsupported_target', 'This Pocket2D frontend targets Lua 5.4');
        const options = { ...parsed.options, outDir: stage, luaTarget: tstl.LuaTarget.Lua54,
            luaLibImport: parsed.options.luaBundle ? tstl.LuaLibImportKind.RequireMinimal : tstl.LuaLibImportKind.Inline,
            noEmitOnError: false,
            emitBOM: false, declaration: false, declarationMap: false, incremental: false,
            composite: false, noImplicitGlobalVariables: true, tstlVerbose: false, measurePerformance: false,
            traceResolution: false, listFiles: false, listEmittedFiles: false, diagnostics: false, extendedDiagnostics: false };
        if (options.noEmit) throw new ToolError('no_emit_project', 'Compile requires an emitting project; prior output was retained');
        // TSTL 1.37.1 treats warning diagnostics as noEmitOnError blockers and
        // can still emit an empty bundle. Check TypeScript errors ourselves,
        // emit only into staging, then gate promotion on every remaining error.
        const program = ts.createProgram(parsed.fileNames, options);
        const beforeEmit = ts.getPreEmitDiagnostics(program);
        diagnostics.push(...beforeEmit.map(diagnostic));
        if (beforeEmit.some(value => value.category === ts.DiagnosticCategory.Error))
            return { ok: false, diagnostics, emitted: [] };
        const result = new tstl.Transpiler().emit({ program, writeFile: (fileName, content, bom) => {
            const target = path.resolve(fileName);
            const relative = path.relative(stage, target);
            if (!relative || relative.startsWith(`..${path.sep}`) || relative === '..' || path.isAbsolute(relative))
                throw new ToolError('emit_outside_stage', 'Compiler attempted to emit outside the staging directory');
            fs.mkdirSync(path.dirname(target), { recursive: true });
            fs.writeFileSync(target, (bom ? '\uFEFF' : '') + content, 'utf8');
            emitted.push(relative.split(path.sep).join('/'));
        } });
        diagnostics.push(...result.diagnostics.map(diagnostic));
        if (result.emitSkipped || diagnostics.some(value => value.category === 'error'))
            return { ok: false, diagnostics, emitted: [] };
        if (!emitted.some(value => value.endsWith('.lua')))
            throw new ToolError('no_lua_emitted', 'Project did not emit any Lua gameplay source');
        const licenseSource = path.join(path.dirname(require.resolve('typescript-to-lua/package.json')), 'LICENSE');
        fs.copyFileSync(licenseSource, path.join(stage, 'TSTL-LICENSE.txt'));
        emitted.push('TSTL-LICENSE.txt');
        const uniqueFiles = [...new Set(emitted)].sort();
        fs.writeFileSync(path.join(stage, markerName), JSON.stringify({ generator: 'pocket2d-tstl', format: 1,
            toolchain: versions, files: uniqueFiles }, null, 2) + '\n');
        // Directory promotion is on the same filesystem. Failed compilation
        // never reaches this point; failed promotion attempts restore old output.
        validateOutput(output);
        if (fs.existsSync(output)) {
            backup = fs.mkdtempSync(path.join(parent, '.pocket-tstl-backup-'));
            fs.rmdirSync(backup);
            fs.renameSync(output, backup);
        }
        try { fs.renameSync(stage, output); stage = undefined; }
        catch (error) {
            if (backup) { fs.renameSync(backup, output); backup = undefined; }
            throw error;
        }
        if (backup) {
            try { fs.rmSync(backup, { recursive: true }); }
            catch (error) { diagnostics.push({ category: 'warning', code: 'backup_cleanup_failed',
                message: `Output was promoted; previous generated backup remains at ${backup}: ${error.message}` }); }
            backup = undefined;
        }
        return { ok: true, diagnostics, emitted: uniqueFiles, output, toolchain: versions };
    } catch (error) {
        diagnostics.push(toolDiagnostic(error));
        if (backup) diagnostics.push({ category: 'error', code: 'preserved_backup',
            message: `Previous output remains at ${backup}` });
        return { ok: false, diagnostics, emitted: [] };
    } finally {
        if (stage) {
            try { fs.rmSync(stage, { recursive: true, force: true }); }
            catch (error) { diagnostics.push({ category: 'warning', code: 'stage_cleanup_failed', message: `${stage}: ${error.message}` }); }
        }
        if (lock) {
            try { fs.rmdirSync(lock); }
            catch (error) { diagnostics.push({ category: 'warning', code: 'lock_cleanup_failed', message: `${lock}: ${error.message}` }); }
        }
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const args = process.argv.slice(2);
    let result;
    if (args.length === 1 && args[0] === '--check-toolchain') {
        try { result = { ok: true, toolchain: verifyToolchain(), diagnostics: [] }; }
        catch (error) { result = { ok: false, diagnostics: [toolDiagnostic(error)] }; }
    } else result = args.length === 4 && args[0] === '--project' && args[2] === '--out-dir'
        ? compileProject(args[1], args[3])
        : { ok: false, diagnostics: [{ category: 'error', code: 'usage',
            message: 'node compile.mjs --project PATH/tsconfig.json --out-dir DEDICATED_OUTPUT_DIR (or --check-toolchain)' }], emitted: [] };
    process.stdout.write(JSON.stringify(result) + os.EOL);
    process.exitCode = result.ok ? 0 : 1;
}
