import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';

const source = readFileSync(new URL('../public/client.js', import.meta.url), 'utf8');
const flush = () => new Promise(resolve => setImmediate(resolve));
function deferred() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
}
function proof() {
    const bytes = new Uint8Array([1, 2, 3]);
    return { id: 'AQID', rawId: bytes, type: 'public-key', getClientExtensionResults: () => ({}),
        response: { clientDataJSON: bytes, attestationObject: bytes, authenticatorData: bytes,
            signature: bytes, userHandle: null, getTransports: () => ['internal'] } };
}
function fixture({ registration = false, fragment = '', registered = false } = {}) {
    const elements = new Map(), timers = new Map(), events = new Map(), overrides = new Map();
    const trace = [], calls = [], historyCalls = [], credentials = [];
    let timerId = 0;
    const state = { registered, assertionVerified: false };
    const location = { pathname: registration ? '/register' : '/verify', search: '', hash: fragment };
    for (const id of ['status', 'state-title', 'result', 'perform', 'join', 'code', 'purpose',
        'join-controls', 'check', 'step-setup', 'step-approval', 'step-verified']) {
        elements.set(id, { hidden: id === 'perform' || id === 'check', value: '', textContent: '', disabled: false,
            dataset: {}, attributes: new Map(), addEventListener(event, callback) { this[event] = callback; },
            setAttribute(name, value) { this.attributes.set(name, value); }, removeAttribute(name) { this.attributes.delete(name); } });
    }
    const options = registration ? { challenge: 'AQ', user: { id: 'Ag' } }
        : { challenge: 'AQ', allowCredentials: [{ id: 'Aw' }] };
    const storage = new Proxy({}, { get() { assert.fail('Join codes must not enter browser storage'); } });
    runInNewContext(source, {
        location,
        history: { replaceState(...args) { trace.push('history'); historyCalls.push(args); location.hash = ''; } },
        document: { getElementById(id) { trace.push('dom'); return elements.get(id); } },
        localStorage: storage, sessionStorage: storage,
        console: new Proxy({}, { get() { assert.fail('The demo must not log ceremony or join data'); } }),
        atob, btoa, Uint8Array, AbortController,
        setTimeout(callback, delay) { const id = ++timerId; timers.set(id, { callback, delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
        addEventListener(event, callback) { events.set(event, callback); },
        navigator: { credentials: {
            async create(value) { credentials.push(['create', value]); return overrides.has('create') ? overrides.get('create')() : proof(); },
            async get(value) { credentials.push(['get', value]); return overrides.has('get') ? overrides.get('get')() : proof(); },
        } },
        fetch: async (path, init) => {
            trace.push('fetch'); calls.push({ path, body: JSON.parse(init.body) });
            if (overrides.has(path)) return overrides.get(path)(init);
            const value = path === '/api/options' ? structuredClone(options)
                : path === '/api/verify' ? { verified: true } : { ...state };
            return { ok: true, json: async () => value };
        },
    });
    return { trace, calls, historyCalls, credentials, timers, events, state, overrides, location,
        element: id => elements.get(id),
        poll: () => [...timers.entries()].find(([, timer]) => timer.delay === 2000),
        async tick() {
            const entry = [...timers.entries()].find(([, timer]) => timer.delay === 2000);
            assert.ok(entry, 'Expected one scheduled setup check');
            timers.delete(entry[0]); await entry[1].callback();
        },
        async join() { elements.get('code').value = 'synthetic-manual-code'; await elements.get('join').click(); },
    };
}

test('fragment is removed before DOM or requests, joins once, and never starts a ceremony', async () => {
    const code = 'a'.repeat(43), f = fixture({ fragment: `#join=${'a'.repeat(43)}`, registered: true });
    await flush();
    assert.equal(f.trace[0], 'history');
    assert.equal(f.location.hash, '');
    assert.deepEqual(f.historyCalls, [[null, '', '/verify']]);
    assert.deepEqual(f.calls, [{ path: '/api/join', body: { code } }]);
    assert.equal(f.element('join-controls').hidden, true);
    assert.equal(f.element('code').value, '');
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
    assert.equal(f.credentials.length, 0);
    assert.equal(f.timers.size, 0);
});
test('invalid fragments are removed without submitting them; manual join remains available', async () => {
    for (const fragment of ['#join=short', `#join=${'a'.repeat(129)}`, `#join=${'a'.repeat(32)}&other=x`, '#join=%41'.repeat(32)]) {
        const f = fixture({ fragment }); await flush();
        assert.equal(f.trace[0], 'history'); assert.equal(f.location.hash, '');
        assert.equal(f.calls.length, 0); assert.equal(f.element('join-controls').hidden, false);
    }
});
test('tablet joins once, clears its code, waits for phone setup, then becomes ready without a ceremony', async () => {
    const f = fixture(); await f.join();
    assert.equal(f.element('join-controls').hidden, true); assert.equal(f.element('code').value, '');
    assert.equal(f.element('perform').hidden, true); assert.equal(f.element('state-title').textContent, 'Set up your phone');
    await f.tick(); assert.equal(f.element('perform').hidden, true);
    f.state.registered = true; await f.tick();
    assert.equal(f.element('perform').hidden, false); assert.equal(f.element('perform').textContent, 'Test phone passkey');
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
    assert.equal(f.calls.filter(call => call.path === '/api/join').length, 1);
    assert.equal(f.credentials.length, 0); assert.equal(f.timers.size, 0);
});
test('a different session previously verifying never makes this page show PASS', async () => {
    const f = fixture({ registered: true }); f.state.assertionVerified = true; await f.join();
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
    assert.notEqual(f.element('result').dataset.outcome, 'pass'); assert.equal(f.credentials.length, 0);
});
test('join failure keeps manual retry available and never renders raw errors', async () => {
    const f = fixture({ registered: true });
    f.overrides.set('/api/join', () => { throw new Error('sensitive-response'); }); await f.join();
    assert.equal(f.element('join-controls').hidden, false); assert.equal(f.element('join').disabled, false);
    assert.equal(f.element('state-title').textContent, 'Could not join this test');
    assert.doesNotMatch(f.element('status').textContent, /sensitive-response/);
    f.overrides.delete('/api/join'); await f.join();
    assert.equal(f.element('join-controls').hidden, true); assert.equal(f.element('perform').hidden, false);
});
test('cancel, verification failure and explicit retry request fresh options; only verified true passes', async () => {
    const f = fixture({ registered: true }); await f.join();
    f.overrides.set('get', () => { throw Object.assign(new Error('private-authenticator-error'), { name: 'NotAllowedError' }); });
    await f.element('perform').click();
    assert.equal(f.element('state-title').textContent, 'Not completed'); assert.equal(f.element('perform').disabled, false);
    assert.equal(f.calls.filter(call => call.path === '/api/verify').length, 0);
    f.overrides.delete('get'); f.overrides.set('/api/verify', () => ({ ok: true, json: async () => ({ verified: false }) }));
    await f.element('perform').click();
    assert.equal(f.element('state-title').textContent, 'FAIL — not verified'); assert.notEqual(f.element('result').dataset.outcome, 'pass');
    f.overrides.delete('/api/verify'); await f.element('perform').click();
    assert.equal(f.element('state-title').textContent, 'PASS'); assert.equal(f.element('result').dataset.outcome, 'pass');
    assert.equal(f.calls.filter(call => call.path === '/api/options').length, 3); assert.equal(f.credentials.length, 3);
    for (const step of ['step-setup', 'step-approval', 'step-verified']) assert.equal(f.element(step).dataset.state, 'complete');
});
test('missing verification result and rejected responses cannot display PASS', async () => {
    for (const response of [{ ok: true, json: async () => ({}) }, { ok: false }]) {
        const f = fixture({ registered: true }); await f.join(); f.overrides.set('/api/verify', () => response);
        await f.element('perform').click();
        assert.equal(f.element('state-title').textContent, 'FAIL — not verified'); assert.equal(f.element('perform').disabled, false);
    }
});
test('pending verification shows checking, blocks duplicate ceremonies, and cannot show PASS early', async () => {
    const f = fixture({ registered: true }); await f.join(); const gate = deferred();
    f.overrides.set('/api/verify', () => gate.promise);
    const pending = f.element('perform').click(); await flush();
    assert.equal(f.element('state-title').textContent, 'Checking your passkey…'); assert.equal(f.element('perform').disabled, true);
    await f.element('perform').click(); assert.equal(f.credentials.length, 1);
    gate.resolve({ ok: true, json: async () => ({ verified: true }) }); await pending;
    assert.equal(f.element('state-title').textContent, 'PASS');
});
test('phone registration needs a button click, creates only, and ends ready without claiming PASS', async () => {
    const f = fixture({ registration: true, fragment: `#join=${'a'.repeat(32)}` }); await flush();
    assert.equal(f.element('state-title').textContent, 'Create a test passkey'); assert.equal(f.credentials.length, 0);
    await f.element('perform').click();
    assert.deepEqual(f.credentials.map(([method]) => method), ['create']);
    assert.equal(f.element('state-title').textContent, 'Phone is ready'); assert.equal(f.element('perform').hidden, true);
    assert.notEqual(f.element('result').dataset.outcome, 'pass'); assert.equal(f.timers.size, 0);
});
test('a failed setup check stops polling and offers a bounded explicit retry', async () => {
    const f = fixture(); await f.join(); f.overrides.set('/api/status', () => { throw new Error('private-network-error'); });
    await f.tick();
    assert.equal(f.element('state-title').textContent, 'Could not check phone setup');
    assert.equal(f.element('check').hidden, false); assert.equal(f.timers.size, 0);
    f.overrides.delete('/api/status'); f.state.registered = true; f.element('check').click(); await f.tick();
    assert.equal(f.element('state-title').textContent, 'Phone is ready'); assert.equal(f.timers.size, 0);
});
test('setup polling is finite and never starts an authenticator', async () => {
    const f = fixture(); await f.join();
    for (let count = 0; count < 90; count++) await f.tick();
    assert.equal(f.calls.filter(call => call.path === '/api/status').length, 90); assert.equal(f.timers.size, 0);
    assert.equal(f.element('check').hidden, false); assert.equal(f.element('state-title').textContent, 'Still waiting for your phone');
    assert.equal(f.credentials.length, 0);
});
test('slow setup checks never overlap or accumulate timers', async () => {
    const f = fixture(); await f.join(); const gate = deferred(); f.overrides.set('/api/status', () => gate.promise);
    const pending = f.tick(); await flush();
    assert.equal(f.poll(), undefined); assert.equal(f.calls.filter(call => call.path === '/api/status').length, 1);
    f.element('check').click(); assert.equal(f.poll(), undefined);
    gate.resolve({ ok: true, json: async () => ({ registered: false }) }); await pending;
    assert.equal([...f.timers.values()].filter(timer => timer.delay === 2000).length, 1);
});
test('a stalled status request times out and stops automatic polling', async () => {
    const f = fixture(); await f.join();
    f.overrides.set('/api/status', init => new Promise((_resolve, reject) => {
        init.signal.addEventListener('abort', () => reject(new Error('Aborted')));
    }));
    const pending = f.tick(); await flush();
    const timeout = [...f.timers.entries()].find(([, timer]) => timer.delay === 10000);
    assert.ok(timeout); f.timers.delete(timeout[0]); timeout[1].callback(); await pending;
    assert.equal(f.element('state-title').textContent, 'Could not check phone setup'); assert.equal(f.timers.size, 0);
});
test('leaving the page cancels polling and late status responses cannot restart it', async () => {
    const f = fixture(); await f.join(); const gate = deferred(); f.overrides.set('/api/status', () => gate.promise);
    const pending = f.tick(); await flush(); f.events.get('pagehide')();
    gate.resolve({ ok: true, json: async () => ({ registered: false }) }); await pending;
    assert.equal(f.timers.size, 0); assert.equal(f.credentials.length, 0);
});

test('back-forward restoration resumes the cookie session without another code or automatic ceremony', async () => {
    const f = fixture({ registered: true }); await f.join();
    f.events.get('pagehide')();
    await f.events.get('pageshow')({ persisted: true });
    assert.equal(f.element('join-controls').hidden, true);
    assert.equal(f.element('code').value, '');
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
    assert.equal(f.element('perform').disabled, false);
    assert.equal(f.calls.filter(call => call.path === '/api/join').length, 1);
    assert.equal(f.calls.filter(call => call.path === '/api/status').length, 1);
    assert.equal(f.credentials.length, 0);
    await f.element('perform').click();
    assert.equal(f.element('state-title').textContent, 'PASS');
});

test('restored unregistered sessions wait again; expired sessions expose manual recovery', async () => {
    const f = fixture(); await f.join();
    f.events.get('pagehide')(); await f.events.get('pageshow')({ persisted: true });
    assert.equal(f.element('state-title').textContent, 'Set up your phone');
    assert.equal(f.element('join-controls').hidden, true); assert.ok(f.poll());
    f.events.get('pagehide')();
    f.overrides.set('/api/status', () => ({ ok: false }));
    await f.events.get('pageshow')({ persisted: true });
    assert.equal(f.element('state-title').textContent, 'Rejoin this test');
    assert.equal(f.element('join-controls').hidden, false);
    assert.equal(f.element('join').disabled, false);
    assert.equal(f.element('perform').hidden, true);
    assert.equal(f.timers.size, 0); assert.equal(f.credentials.length, 0);
});

test('an old authenticator is aborted and its late response cannot verify after restoration', async () => {
    const f = fixture({ registered: true }); await f.join(); const gate = deferred();
    f.overrides.set('get', () => gate.promise);
    const pending = f.element('perform').click(); await flush();
    const signal = f.credentials[0][1].signal;
    f.events.get('pagehide')(); assert.equal(signal.aborted, true);
    await f.events.get('pageshow')({ persisted: true });
    gate.resolve(proof()); await pending;
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
    assert.equal(f.calls.filter(call => call.path === '/api/verify').length, 0);
    assert.equal(f.element('perform').disabled, false);
    f.overrides.delete('get'); await f.element('perform').click();
    assert.equal(f.element('state-title').textContent, 'PASS');
});

test('a late successful verification cannot display PASS or unblock a newer ceremony', async () => {
    const f = fixture({ registered: true }); await f.join(); const old = deferred(), next = deferred();
    f.overrides.set('/api/verify', () => old.promise);
    const oldPending = f.element('perform').click(); await flush();
    f.events.get('pagehide')(); await f.events.get('pageshow')({ persisted: true });
    f.overrides.delete('/api/verify'); f.overrides.set('get', () => next.promise);
    const nextPending = f.element('perform').click(); await flush();
    old.resolve({ ok: true, json: async () => ({ verified: true }) }); await oldPending;
    assert.equal(f.element('state-title').textContent, 'Waiting for your phone…');
    assert.equal(f.element('perform').disabled, true);
    assert.notEqual(f.element('result').dataset.outcome, 'pass');
    next.resolve(proof()); await nextPending;
    assert.equal(f.element('state-title').textContent, 'PASS');
});

test('stale setup and options responses cannot change restored state or start an authenticator', async () => {
    const waiting = fixture(); await waiting.join(); const oldStatus = deferred(); let checks = 0;
    waiting.overrides.set('/api/status', () => ++checks === 1 ? oldStatus.promise
        : { ok: true, json: async () => ({ registered: true }) });
    const oldCheck = waiting.tick(); await flush();
    waiting.events.get('pagehide')(); await waiting.events.get('pageshow')({ persisted: true });
    oldStatus.resolve({ ok: true, json: async () => ({ registered: false }) }); await oldCheck;
    assert.equal(waiting.element('state-title').textContent, 'Phone is ready');
    assert.equal(waiting.timers.size, 0);
    const f = fixture({ registered: true }); await f.join(); const options = deferred();
    f.overrides.set('/api/options', () => options.promise);
    const oldRequest = f.element('perform').click(); await flush();
    f.events.get('pagehide')(); await f.events.get('pageshow')({ persisted: true });
    options.resolve({ ok: true, json: async () => ({ challenge: 'AQ' }) }); await oldRequest;
    assert.equal(f.credentials.length, 0);
    assert.equal(f.element('state-title').textContent, 'Phone is ready');
});
