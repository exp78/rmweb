import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { createAcceptanceServer, configuration } from '../server.js';
import { HarnessError, testOrigin } from '../harness.js';
import { testCredential } from './credentials.js';
const origin = 'http://localhost:8089';
async function fixture(t, options = { origin }) {
    const server = createAcceptanceServer(options);
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(() => { server.closeAllConnections(); server.close(); });
    return (path, data, { cookie, headers = {}, method = 'POST' } = {}) => new Promise((resolve, reject) => {
        const bytes = data === undefined ? '' : JSON.stringify(data);
        const request = http.request({ host: '127.0.0.1', port: server.address().port, path, method,
            headers: { Host: new URL(options.origin).host, Origin: options.origin, 'Content-Type': 'application/json',
                ...(cookie ? { Cookie: cookie } : {}), ...headers } }, response => {
            const chunks = []; response.on('data', chunk => chunks.push(chunk));
            response.on('end', () => resolve({ status: response.statusCode, headers: response.headers,
                text: Buffer.concat(chunks).toString(), json: () => JSON.parse(Buffer.concat(chunks)) }));
        });
        request.on('error', reject); request.end(bytes);
    });
}
const cookie = result => result.headers['set-cookie'][0].split(';')[0];
test('phone registration and separate tablet session verify through real HTTP server', async t => {
    const call = await fixture(t);
    const phone = cookie(await call('/api/join', {}));
    const tablet = cookie(await call('/api/join', {}));
    const key = testCredential();
    const options = (await call('/api/options', { kind: 'register' }, { cookie: phone })).json();
    assert.equal((await call('/api/verify', { kind: 'register', credential: key.registration(options, origin) }, { cookie: phone })).status, 200);
    const request = (await call('/api/options', { kind: 'authenticate' }, { cookie: tablet })).json();
    assert.equal((await call('/api/verify', { kind: 'authenticate', credential: key.assertion(request, origin) }, { cookie: tablet })).status, 200);
    assert.deepEqual((await call('/api/status', {}, { cookie: tablet })).json(), { registered: true, assertionVerified: true });
});
test('rejects off-origin, wrong host, cross-site, iframe and unauthenticated requests', async t => {
    const call = await fixture(t);
    for (const headers of [{ Origin: 'https://other.test' }, { Host: 'other.test' },
        { 'Sec-Fetch-Site': 'cross-site' }, { 'Sec-Fetch-Dest': 'iframe' }]) {
        const response = await call('/api/join', {}, { headers });
        assert.equal(response.status, 400); assert.equal(response.text, '{"error":"Test request rejected"}');
    }
    assert.equal((await call('/api/status', {})).status, 400);
    assert.equal((await call('/api/join?redirect=https://other.test', {})).status, 400);
});
test('request size, session count and static routes stay bounded', async t => {
    const call = await fixture(t);
    assert.equal((await call('/api/join', { code: 'x'.repeat(65537) })).status, 400);
    for (let i = 0; i < 4; i++) assert.equal((await call('/api/join', {})).status, 200);
    assert.equal((await call('/api/join', {})).status, 400);
    for (const path of ['/register', '/verify', '/client.js', '/style.css']) {
        const response = await call(path, undefined, { method: 'GET' });
        assert.equal(response.status, 200); assert.equal(response.headers['cache-control'], 'no-store');
        assert.match(response.headers['content-security-policy'], /frame-ancestors 'none'/);
        assert.equal(response.headers['referrer-policy'], 'no-referrer');
    }
    assert.equal((await call('/../server.js', undefined, { method: 'GET' })).status, 400);
});
test('session cookies stay HttpOnly and same-site, and joins reuse a valid session', async t => {
    const call = await fixture(t); const initial = await call('/api/join', {});
    assert.match(initial.headers['set-cookie'][0], /HttpOnly; SameSite=Strict/);
    const same = await call('/api/join', {}, { cookie: cookie(initial) });
    assert.equal(cookie(same), cookie(initial));
});
test('configuration is loopback-only by default and LAN requires HTTPS plus explicit access code', () => {
    assert.equal(configuration({}).bind, '127.0.0.1');
    assert.equal(configuration({}).origin, origin);
    for (const env of [{ TEST_BIND: '0.0.0.0' }, { TEST_ORIGIN: 'http://example.test' },
        { TEST_ORIGIN: 'https://example.test' }, { TEST_ORIGIN: 'https://example.test', TEST_ACCESS_CODE: 'short' }])
        assert.throws(() => configuration(env), HarnessError);
    for (const value of ['https://user@example.test', 'https://example.test/', 'https://example.test?x',
        'https://example.test:443', 'https://example.test:0', 'https://single', 'https://127.0.0.1', 'file:///tmp/test', 'https://example.test#x']) assert.throws(() => testOrigin(value), HarnessError);
});
test('explicit local proxy requires HTTPS, an access code, and a loopback bind', () => {
    const proxy = { TEST_LOOPBACK_PROXY: '1', TEST_ORIGIN: 'https://disposable.trycloudflare.com',
        TEST_ACCESS_CODE: 'a'.repeat(43) };
    assert.deepEqual(configuration(proxy), { origin: proxy.TEST_ORIGIN, code: proxy.TEST_ACCESS_CODE,
        bind: '127.0.0.1', port: 0, tls: undefined, loopbackProxy: true });
    assert.equal(configuration({ ...proxy, TEST_PORT: '12345' }).port, 12345);
    for (const fields of [{ TEST_BIND: '0.0.0.0' }, { TEST_BIND: '192.168.1.2' },
        { TEST_ORIGIN: 'http://localhost:8089' }, { TEST_ACCESS_CODE: '' },
        { TEST_TLS_KEY: '/unused/key' }, { TEST_TLS_CERT: '/unused/cert' },
        { TEST_LOOPBACK_PROXY: 'yes' }, { TEST_PORT: '65536' }, { TEST_PORT: '1.5' }, { TEST_PORT: '' }])
        assert.throws(() => configuration({ ...proxy, ...fields }), HarnessError);
    assert.throws(() => createAcceptanceServer({ origin: proxy.TEST_ORIGIN, code: proxy.TEST_ACCESS_CODE,
        loopbackProxy: true, bind: '0.0.0.0' }), HarnessError);
    assert.throws(() => configuration({ TEST_PORT: '1234' }), HarnessError);
});
test('local HTTP proxy preserves exact external Host and Origin and sets Secure cookies', async t => {
    const external = 'https://disposable.trycloudflare.com';
    const code = 'a'.repeat(43);
    const call = await fixture(t, { origin: external, code, loopbackProxy: true });
    assert.equal((await call('/api/join', { code: 'wrong' })).status, 400);
    const accepted = await call('/api/join', { code }, { headers: {
        Forwarded: 'host=untrusted.test;proto=http', 'X-Forwarded-Host': 'untrusted.test', 'X-Forwarded-Proto': 'http' } });
    assert.equal(accepted.status, 200);
    assert.match(accepted.headers['set-cookie'][0], /HttpOnly; SameSite=Strict; Secure$/);
    for (const headers of [{ Host: '127.0.0.1', 'X-Forwarded-Host': new URL(external).host },
        { Origin: 'http://disposable.trycloudflare.com', 'X-Forwarded-Proto': 'https' },
        { Origin: 'https://other.test', Forwarded: `host=${new URL(external).host};proto=https` }])
        assert.equal((await call('/api/join', { code }, { headers })).status, 400);
    const key = testCredential();
    const joined = cookie(accepted);
    const options = (await call('/api/options', { kind: 'register' }, { cookie: joined })).json();
    assert.equal((await call('/api/verify', { kind: 'register', credential: key.registration(options, external) },
        { cookie: joined })).status, 200);
});
