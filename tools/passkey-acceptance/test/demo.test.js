import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import http from 'node:http';
import { mkdtemp, mkdir, writeFile, readFile, stat, rm, access, realpath } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { cloudflaredPath, DemoError, startDemo, tunnelOrigin } from '../demo.js';

async function fixture(t) {
    const directory = await realpath(await mkdtemp(join(tmpdir(), 'rmweb-demo-test-')));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const bin = join(directory, 'bin'); await mkdir(bin);
    await writeFile(join(bin, 'cloudflared'), '#!/bin/sh\nexit 99\n', { mode: 0o700 });
    let resolveSpawn;
    const spawned = new Promise(accept => { resolveSpawn = accept; });
    const child = new EventEmitter();
    child.stdout = new PassThrough(); child.stderr = new PassThrough();
    child.exitCode = null; child.signalCode = null; child.kills = [];
    child.kill = signal => {
        child.kills.push(signal); child.signalCode = signal;
        queueMicrotask(() => child.emit('close', null, signal)); return true;
    };
    const spawnChild = (executable, args, options) => {
        resolveSpawn({ executable, args, options, child }); return child;
    };
    const options = { sessionFile: join(directory, 'private-session.json'), searchPath: bin,
        startupMs: 1000, lifetimeMs: 3000, spawnChild };
    return { directory, bin, child, spawned, options };
}
function request(port, host, path = '/register') {
    return new Promise((accept, reject) => {
        http.get({ host: '127.0.0.1', port, path, headers: { Host: host } }, response => {
            response.resume(); response.on('end', () => accept(response.statusCode));
        }).on('error', reject);
    });
}
const announced = 'https://disposable-rmweb.trycloudflare.com';
test('quick tunnel origin accepts only canonical HTTPS assigned subdomains', () => {
    assert.equal(tunnelOrigin(announced), announced);
    for (const candidate of ['https://trycloudflare.com', 'http://test.trycloudflare.com',
        'https://test.trycloudflare.com.evil.test', 'https://test.trycloudflare.com:443',
        'https://test.trycloudflare.com/', 'https://test.trycloudflare.com#private',
        'https://user@test.trycloudflare.com', 'https://test.other.test', 'https://TEST.trycloudflare.com'])
        assert.throws(() => tunnelOrigin(candidate), DemoError);
});
test('PATH resolution requires an executable file and skips relative search entries', async t => {
    const f = await fixture(t);
    assert.equal(await cloudflaredPath(`.:${f.bin}`), join(f.bin, 'cloudflared'));
    await assert.rejects(cloudflaredPath('.'), DemoError);
});
test('real local socket stays gated until origin; session is private and cleanup stops both children', async t => {
    const f = await fixture(t);
    const starting = startDemo(f.options);
    const invoked = await f.spawned;
    const target = new URL(invoked.args[invoked.args.indexOf('--url') + 1]);
    assert.equal(target.hostname, '127.0.0.1');
    assert.equal(invoked.options.shell, false);
    assert.equal(invoked.options.env.TEST_ACCESS_CODE, undefined);
    assert.equal(await request(target.port, 'untrusted.test'), 503);
    f.child.stdout.write('private raw diagnostics are never forwarded\n');
    f.child.stderr.write(`notice | ${announced}\n`);
    const demo = await starting;
    t.after(() => demo.close());
    assert.equal(await request(target.port, new URL(announced).host), 200);
    assert.equal(await request(target.port, 'untrusted.test'), 400);
    const session = JSON.parse(await readFile(demo.sessionFile, 'utf8'));
    assert.equal(session.origin, announced); assert.equal(session.pid, process.pid);
    const registration = new URL(session.registrationURL), verification = new URL(session.verificationURL);
    assert.equal(registration.pathname, '/register'); assert.equal(verification.pathname, '/verify');
    assert.match(registration.hash, /^#join=[A-Za-z0-9_-]{43}$/);
    assert.equal(verification.hash, registration.hash);
    assert.equal((await stat(demo.sessionFile)).mode & 0o777, 0o600);
    await demo.close();
    assert.deepEqual(await demo.done, { failed: false });
    assert.deepEqual(f.child.kills, ['SIGTERM']);
    await assert.rejects(access(demo.sessionFile));
    await assert.rejects(access(invoked.options.env.HOME));
    await assert.rejects(request(target.port, new URL(announced).host));
});
test('startup timeout removes its private session and terminates the tunnel', async t => {
    const f = await fixture(t);
    const starting = startDemo({ ...f.options, startupMs: 20 });
    await f.spawned;
    await assert.rejects(starting, DemoError);
    assert.deepEqual(f.child.kills, ['SIGTERM']);
    await assert.rejects(access(f.options.sessionFile));
});
test('abort after startup closes server, tunnel and private session', async t => {
    const f = await fixture(t), controller = new AbortController();
    const starting = startDemo({ ...f.options, signal: controller.signal });
    await f.spawned; f.child.stderr.write(`${announced}\n`);
    const demo = await starting; controller.abort();
    assert.deepEqual(await demo.done, { failed: false });
    await assert.rejects(access(demo.sessionFile));
    assert.deepEqual(f.child.kills, ['SIGTERM']);
});
test('bounded stderr rejects oversized lines and conflicting assigned origins', async t => {
    for (const value of ['x'.repeat(8193), `${announced}\nhttps://another.trycloudflare.com\n`]) {
        const f = await fixture(t);
        const starting = startDemo(f.options);
        await f.spawned; f.child.stderr.write(value);
        await assert.rejects(starting, DemoError);
        assert.deepEqual(f.child.kills, ['SIGTERM']);
        await assert.rejects(access(f.options.sessionFile));
    }
});
test('existing session files are preserved and never used', async t => {
    const f = await fixture(t);
    await writeFile(f.options.sessionFile, 'preserve');
    await assert.rejects(startDemo(f.options), DemoError);
    assert.equal(await readFile(f.options.sessionFile, 'utf8'), 'preserve');
});
