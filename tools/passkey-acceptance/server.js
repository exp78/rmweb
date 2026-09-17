import http from 'node:http';
import https from 'node:https';
import { isIP } from 'node:net';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { timingSafeEqual } from 'node:crypto';
import { AcceptanceHarness, BODY_LIMIT, RUN_MS, HarnessError, testOrigin } from './harness.js';

const assets = new Map(['/register', '/verify'].map(path => [path,
    ['text/html; charset=utf-8', readFileSync(new URL('./public/index.html', import.meta.url))]]));
assets.set('/client.js', ['text/javascript; charset=utf-8', readFileSync(new URL('./public/client.js', import.meta.url))]);
assets.set('/style.css', ['text/css; charset=utf-8', readFileSync(new URL('./public/style.css', import.meta.url))]);
export function configuration(env) {
    const origin = env.TEST_ORIGIN || 'http://localhost:8089';
    const url = testOrigin(origin);
    const bind = env.TEST_BIND || '127.0.0.1';
    if (!isIP(bind)) throw new HarnessError();
    const secure = url.protocol === 'https:';
    const code = env.TEST_ACCESS_CODE || '';
    if (env.TEST_LOOPBACK_PROXY !== undefined && env.TEST_LOOPBACK_PROXY !== '1') throw new HarnessError();
    const loopbackProxy = env.TEST_LOOPBACK_PROXY === '1';
    if (loopbackProxy && (!secure || !loopback(bind) || env.TEST_TLS_KEY || env.TEST_TLS_CERT)) throw new HarnessError();
    if (secure && (!/^[A-Za-z0-9_-]{32,128}$/.test(code)
        || (!loopbackProxy && (!env.TEST_TLS_KEY || !env.TEST_TLS_CERT)))) throw new HarnessError();
    if (!secure && (bind !== '127.0.0.1' || env.TEST_TLS_KEY || env.TEST_TLS_CERT || code)) throw new HarnessError();
    if (!loopbackProxy && env.TEST_PORT !== undefined) throw new HarnessError();
    const port = loopbackProxy ? Number(env.TEST_PORT ?? '0') : Number(url.port || (secure ? 443 : 80));
    if (!Number.isInteger(port) || port < 0 || port > 65535
        || (loopbackProxy && !/^(?:0|[1-9][0-9]{0,4})$/.test(env.TEST_PORT ?? '0'))) throw new HarnessError();
    return { origin, bind, port, code, loopbackProxy,
        tls: secure && !loopbackProxy ? { key: readFileSync(env.TEST_TLS_KEY), cert: readFileSync(env.TEST_TLS_CERT), minVersion: 'TLSv1.2' } : undefined };
}
function loopback(address) { return address === '127.0.0.1' || address === '::1'; }
function equal(a, b) {
    return typeof a === 'string' && Buffer.byteLength(a) === Buffer.byteLength(b)
        && timingSafeEqual(Buffer.from(a), Buffer.from(b));
}
function session(request) {
    const cookie = request.headers.cookie || '';
    const match = /(?:^|; *)acceptance=([A-Za-z0-9_-]{43})(?:;|$)/.exec(cookie);
    return match?.[1];
}
async function body(request) {
    if (request.headers['content-type'] !== 'application/json'
        || Number(request.headers['content-length'] || 0) > BODY_LIMIT) throw new HarnessError();
    const chunks = []; let length = 0;
    for await (const chunk of request) {
        length += chunk.length;
        if (length > BODY_LIMIT) throw new HarnessError();
        chunks.push(chunk);
    }
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
}
export function createAcceptanceHandler({ origin, code = '', tls, loopbackProxy = false, bind = '127.0.0.1',
    harness = new AcceptanceHarness({ origin }) }) {
    const url = testOrigin(origin);
    const secure = url.protocol === 'https:';
    if (loopbackProxy && (!secure || tls || !loopback(bind))) throw new HarnessError();
    if (secure !== Boolean(tls || loopbackProxy)
        || (secure && !/^[A-Za-z0-9_-]{32,128}$/.test(code))) throw new HarnessError();
    let active = 0, joins = 0;
    const handler = async (request, response) => {
        response.setHeader('Cache-Control', 'no-store');
        response.setHeader('Referrer-Policy', 'no-referrer');
        response.setHeader('X-Content-Type-Options', 'nosniff');
        response.setHeader('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        response.setHeader('Permissions-Policy', 'publickey-credentials-create=(self), publickey-credentials-get=(self)');
        const json = (status, value) => {
            response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
            response.end(JSON.stringify(value));
        };
        let counted = false;
        try {
            // The proxy hop is explicitly local. Forwarded headers never
            // establish the origin, HTTPS status, Host, or client identity.
            if (loopbackProxy && (!loopback(request.socket.localAddress)
                || !loopback(request.socket.remoteAddress))) throw new HarnessError();
            if (request.headers.host !== url.host || request.headers['sec-fetch-site'] === 'cross-site'
                || request.headers['sec-fetch-dest'] === 'iframe') throw new HarnessError();
            if (request.method === 'GET' && assets.has(request.url)) {
                const [type, bytes] = assets.get(request.url);
                response.writeHead(200, { 'Content-Type': type }); response.end(bytes); return;
            }
            if (request.method !== 'POST' || request.headers.origin !== origin || active >= 8
                || !['/api/join', '/api/status', '/api/options', '/api/verify'].includes(request.url)) throw new HarnessError();
            active++; counted = true;
            const data = await body(request);
            if (!data || typeof data !== 'object' || Array.isArray(data)) throw new HarnessError();
            let token = session(request);
            if (request.url === '/api/join') {
                if (++joins > 64 || (code && !equal(data.code, code))) throw new HarnessError();
                try { harness.status(token); } catch { token = harness.openSession(); }
                response.setHeader('Set-Cookie', `acceptance=${token}; Path=/; HttpOnly; SameSite=Strict${secure ? '; Secure' : ''}`);
                json(200, harness.status(token));
            } else if (request.url === '/api/status') json(200, harness.status(token));
            else if (request.url === '/api/options') json(200, await harness.options(token, data.kind));
            else json(200, await harness.verify(token, data.kind, data.credential));
        } catch {
            // Library exceptions can contain ceremony values. Never serialize/log them.
            if (!response.headersSent && !response.destroyed) json(400, { error: 'Test request rejected' });
            request.resume();
        } finally { if (counted) active--; }
    };
    return handler;
}
export function createBoundedServer(handler, tls) {
    const options = { requestTimeout: 15000, headersTimeout: 10000, keepAliveTimeout: 5000, maxHeaderSize: 8192 };
    const server = tls ? https.createServer({ ...options, ...tls }, handler) : http.createServer(options, handler);
    server.maxConnections = 8; server.maxHeadersCount = 32; server.maxRequestsPerSocket = 32;
    server.on('clientError', (_error, socket) => { socket.destroy(); });
    return server;
}
export function createAcceptanceServer(options) {
    return createBoundedServer(createAcceptanceHandler(options), options.tls);
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    try {
        if (process.argv.length !== 2) throw new HarnessError();
        const config = configuration(process.env);
        const server = createAcceptanceServer(config);
        const stop = () => { server.close(); server.closeAllConnections(); };
        const deadline = setTimeout(stop, RUN_MS);
        server.on('close', () => clearTimeout(deadline));
        server.on('error', () => { process.stderr.write('Acceptance server unavailable\n'); stop(); process.exitCode = 1; });
        process.once('SIGTERM', stop); process.once('SIGINT', stop);
        server.listen(config.port, config.bind, () => process.stdout.write('Disposable acceptance server ready\n'));
    } catch { process.stderr.write('Acceptance configuration rejected\n'); process.exitCode = 1; }
}
