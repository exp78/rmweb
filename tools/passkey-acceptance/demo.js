import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { constants } from 'node:fs';
import { access, lstat, mkdtemp, open, realpath, rm, stat, unlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, delimiter, dirname, isAbsolute, join, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createAcceptanceHandler, createBoundedServer } from './server.js';
import { RUN_MS } from './harness.js';

const REPOSITORY = fileURLToPath(new URL('../../', import.meta.url));
const MAX_OUTPUT = 256 * 1024;
const MAX_LINE = 8192;
export class DemoError extends Error {
    constructor() { super('Disposable passkey demo unavailable'); }
}
export function tunnelOrigin(value) {
    if (typeof value !== 'string'
        || !/^https:\/\/[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.trycloudflare\.com$/.test(value)) throw new DemoError();
    return value;
}
export async function cloudflaredPath(search = process.env.PATH || '') {
    for (const directory of search.split(delimiter)) {
        if (!isAbsolute(directory)) continue;
        try {
            const candidate = await realpath(join(directory, 'cloudflared'));
            if (!(await stat(candidate)).isFile()) continue;
            await access(candidate, constants.X_OK);
            return candidate;
        } catch { /* A missing PATH candidate is ordinary. */ }
    }
    throw new DemoError();
}
async function sessionPath(requested, temporary) {
    const supplied = requested ?? join(temporary, 'session.json');
    if (!isAbsolute(supplied) || /[\x00-\x1f\x7f]/.test(supplied)) throw new DemoError();
    const candidate = join(await realpath(dirname(supplied)), basename(supplied));
    const repository = await realpath(REPOSITORY);
    if (candidate === repository || candidate.startsWith(repository + sep)) throw new DemoError();
    return candidate;
}

// Dependency seams are used only by offline process fixtures. The CLI exposes
// a session-file path, never a tunnel executable, URL, token, or server override.
export async function startDemo({ sessionFile, signal, spawnChild = spawn,
    searchPath = process.env.PATH, startupMs = 45000, lifetimeMs = RUN_MS } = {}) {
    let temporary, recordPath, record, recordIdentity, server, child, startup, deadline, killTimer;
    let handler, origin, activation = Promise.resolve(), stopping = false, closePromise, childClosed;
    let resolveReady, rejectReady, resolveDone;
    const ready = new Promise((accept, reject) => { resolveReady = accept; rejectReady = reject; });
    // Attach rejection handling before asynchronous setup can receive a signal.
    ready.catch(() => {});
    const done = new Promise(accept => { resolveDone = accept; });
    const stop = (failed = false) => {
        if (closePromise) return closePromise;
        stopping = true;
        clearTimeout(startup); clearTimeout(deadline);
        handler = undefined;
        closePromise = (async () => {
            const closingServer = server ? new Promise(accept => {
                server.close(() => accept()); server.closeAllConnections();
            }) : Promise.resolve();
            if (child && child.exitCode === null && child.signalCode === null) {
                child.kill('SIGTERM');
                killTimer = setTimeout(() => child.kill('SIGKILL'), 2000);
            }
            await Promise.all([closingServer, childClosed, activation.catch(() => {})]);
            clearTimeout(killTimer);
            if (record) { await record.close().catch(() => {}); record = undefined; }
            if (recordPath && recordIdentity) {
                const current = await lstat(recordPath).catch(() => undefined);
                if (current?.isFile() && current.dev === recordIdentity.dev && current.ino === recordIdentity.ino)
                    await unlink(recordPath).catch(() => {});
            }
            if (temporary) await rm(temporary, { recursive: true, force: true }).catch(() => {});
            signal?.removeEventListener('abort', abort);
            rejectReady(new DemoError());
            resolveDone({ failed });
        })();
        return closePromise;
    };
    const abort = () => { void stop(); };
    try {
        if (!Number.isInteger(startupMs) || startupMs < 1 || startupMs > 60000
            || !Number.isInteger(lifetimeMs) || lifetimeMs < 1 || lifetimeMs > RUN_MS) throw new DemoError();
        const executable = await cloudflaredPath(searchPath);
        temporary = await mkdtemp(join(tmpdir(), 'rmweb-passkey-demo-'));
        recordPath = await sessionPath(sessionFile, temporary);
        record = await open(recordPath, constants.O_CREAT | constants.O_EXCL | constants.O_WRONLY | constants.O_NOFOLLOW, 0o600);
        recordIdentity = await record.stat();
        const code = randomBytes(32).toString('base64url');
        server = createBoundedServer((request, response) => {
            if (handler) return handler(request, response);
            response.writeHead(503, { 'Cache-Control': 'no-store', 'Content-Type': 'text/plain; charset=utf-8' });
            response.end('Disposable test is starting\n'); request.resume();
        });
        await new Promise((accept, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', accept);
        });
        server.on('error', () => { void stop(true); });
        if (signal?.aborted) { await stop(); throw new DemoError(); }
        signal?.addEventListener('abort', abort, { once: true });
        child = spawnChild(executable, ['tunnel', '--no-autoupdate', '--config', '/dev/null',
            '--metrics', '127.0.0.1:0', '--loglevel', 'info', '--url', `http://127.0.0.1:${server.address().port}`],
        { shell: false, stdio: ['ignore', 'pipe', 'pipe'],
            env: { PATH: searchPath || '', HOME: temporary, XDG_CONFIG_HOME: temporary, TMPDIR: temporary, NO_COLOR: '1' } });
        childClosed = new Promise(accept => child.once('close', accept));
        child.once('error', () => { void stop(true); });
        child.once('close', () => { if (!stopping) void stop(true); });
        startup = setTimeout(() => { void stop(true); }, startupMs);
        let outputBytes = 0, pending = '';
        const consume = (chunk, parse) => {
            if (stopping) return;
            outputBytes += chunk.length;
            if (outputBytes > MAX_OUTPUT) { void stop(true); return; }
            if (!parse) return;
            pending += chunk.toString('utf8');
            let newline;
            while ((newline = pending.indexOf('\n')) >= 0) {
                if (newline > MAX_LINE) { void stop(true); return; }
                const line = pending.slice(0, newline); pending = pending.slice(newline + 1);
                for (const candidate of line.match(/https:\/\/[^\s|]+/g) || []) {
                    try { tunnelOrigin(candidate); } catch { continue; }
                    if (origin) {
                        if (origin !== candidate) void stop(true);
                        continue;
                    }
                    origin = candidate;
                    activation = (async () => {
                        handler = createAcceptanceHandler({ origin, code, loopbackProxy: true, bind: '127.0.0.1' });
                        const session = { origin, registrationURL: `${origin}/register#join=${code}`,
                            verificationURL: `${origin}/verify#join=${code}`, pid: process.pid };
                        await record.writeFile(JSON.stringify(session, null, 2) + '\n');
                        await record.sync(); await record.close(); record = undefined;
                        if (stopping) return;
                        clearTimeout(startup);
                        deadline = setTimeout(() => { void stop(); }, lifetimeMs);
                        resolveReady({ sessionFile: recordPath, done, close: () => stop() });
                    })();
                    activation.catch(() => { void stop(true); });
                }
            }
            if (pending.length > MAX_LINE) void stop(true);
        };
        child.stdout.on('data', chunk => consume(chunk, false));
        child.stderr.on('data', chunk => consume(chunk, true));
        return await ready;
    } catch {
        await stop(true);
        throw new DemoError();
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const controller = new AbortController();
    const stop = () => controller.abort();
    process.once('SIGINT', stop); process.once('SIGTERM', stop);
    try {
        const args = process.argv.slice(2);
        if (args.length !== 0 && (args.length !== 2 || args[0] !== '--session-file')) throw new DemoError();
        const demo = await startDemo({ sessionFile: args[1], signal: controller.signal });
        process.stdout.write(`Disposable passkey demo ready\nSession file: ${demo.sessionFile}\n`);
        const result = await demo.done;
        process.stdout.write('Disposable passkey demo stopped\n');
        if (result.failed) process.exitCode = 1;
    } catch {
        process.stderr.write('Disposable passkey demo unavailable\n');
        process.exitCode = controller.signal.aborted ? 0 : 1;
    } finally {
        process.removeListener('SIGINT', stop); process.removeListener('SIGTERM', stop);
    }
}
